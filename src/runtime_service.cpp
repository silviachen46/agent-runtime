#include "agent_runtime/runtime_service.hpp"

#include <utility>

namespace ar {

namespace {

LatencySensitivity latency_from_request(const ScheduledRequest& req) {
    const int target_ms = req.is_resume
        ? req.resume_target_ms
        : req.ttft_target_ms;

    if (target_ms <= 0) {
        return LatencySensitivity::Medium;
    }

    if (target_ms <= 1000) {
        return LatencySensitivity::High;
    }

    if (target_ms <= 3000) {
        return LatencySensitivity::Medium;
    }

    return LatencySensitivity::Low;
}

SessionSpec make_session_spec(const ScheduledRequest& req) {
    SessionSpec spec;
    spec.session_id = req.session_id;
    spec.policy.visibility = UserVisibility::Foreground;
    spec.policy.workload = req.is_resume ? WorkloadKind::Agent : WorkloadKind::Chat;
    spec.policy.latency = latency_from_request(req);
    spec.policy.priority = req.priority;
    spec.slo.ttft_target_ms = req.ttft_target_ms;
    spec.slo.resume_target_ms = req.resume_target_ms;
    spec.slo.deadline_ms = req.deadline_ms;
    return spec;
}

TurnSpec make_turn_spec(const ScheduledRequest& req) {
    TurnSpec spec;
    spec.session_id = req.session_id;
    spec.turn_type = req.is_resume
        ? TurnType::ResumeGenerate
        : TurnType::InitialGenerate;
    spec.max_tokens = req.max_tokens;
    spec.temperature = req.temperature;
    spec.stream = true;
    spec.messages.push_back(Message{
        .role = "user",
        .content = req.prompt
    });
    return spec;
}

std::string output_from_events(const BackendResult& result) {
    std::string output;

    for (const auto& event : result.events) {
        if (event.done) {
            continue;
        }

        if (!output.empty()) {
            output += " ";
        }

        output += event.text;
    }

    return output;
}

ScheduleResponse make_base_response(const ScheduledRequest& req) {
    ScheduleResponse resp;
    resp.request_id = req.request_id;
    resp.session_id = req.session_id;
    resp.workflow_id = req.workflow_id;
    resp.step_id = req.step_id;
    return resp;
}

ScheduleResponse make_completed_response(
    const ScheduledRequest& req,
    const RuntimeRunResult& result
) {
    ScheduleResponse resp = make_base_response(req);
    resp.status = "completed";
    resp.output = output_from_events(result.backend_result);
    resp.queue_wait_ms = result.queue_wait_ms;
    resp.ttft_ms = result.backend_result.ttft_ms;
    resp.total_latency_ms =
        result.queue_wait_ms + result.backend_result.total_latency_ms;
    resp.output_tokens = result.backend_result.output_tokens;

    if (req.deadline_ms > 0) {
        resp.deadline_missed = resp.total_latency_ms > req.deadline_ms;
    }

    return resp;
}

} // namespace

RuntimeService::RuntimeService(
    RuntimeServiceConfig service_config,
    SchedulerConfig scheduler_config,
    MockBackendConfig backend_config
)
    : RuntimeService(
          service_config,
          scheduler_config,
          make_mock_backend(backend_config)
      ) {}

RuntimeService::RuntimeService(
    RuntimeServiceConfig service_config,
    SchedulerConfig scheduler_config,
    std::shared_ptr<Backend> backend
)
    : service_config_(service_config),
      scheduler_config_(scheduler_config),
      runtime_(scheduler_config, std::move(backend)) {
    if (service_config_.max_inflight_backend_requests == 0) {
        service_config_.max_inflight_backend_requests = 1;
    }

    if (service_config_.max_runtime_queue_depth == 0) {
        service_config_.max_runtime_queue_depth = 1;
    }

    dispatcher_ = std::thread(&RuntimeService::dispatcher_loop, this);
}

RuntimeService::RuntimeService(
    SchedulerConfig scheduler_config,
    MockBackendConfig backend_config
)
    : RuntimeService(
          RuntimeServiceConfig{},
          scheduler_config,
          backend_config
      ) {}

RuntimeService::~RuntimeService() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }

    cv_.notify_all();

    if (dispatcher_.joinable()) {
        dispatcher_.join();
    }

    for (auto& execution_thread : execution_threads_) {
        if (execution_thread.thread.joinable()) {
            execution_thread.thread.join();
        }
    }
}

ScheduleResponse RuntimeService::schedule(const ScheduledRequest& req) {
    std::future<RuntimeRunResult> future;

    {
        std::lock_guard<std::mutex> lock(mu_);
        reap_finished_threads_locked();

        if (stopping_ ||
            runtime_.queued_turn_count() >= service_config_.max_runtime_queue_depth) {
            ++rejected_requests_;
            ScheduleResponse resp = make_base_response(req);
            resp.status = "rejected";
            return resp;
        }

        if (!runtime_.get_session(req.session_id).has_value()) {
            runtime_.create_session(make_session_spec(req));
        }

        const auto turn_id = runtime_.submit_turn_with_id(make_turn_spec(req));
        if (!turn_id.has_value()) {
            ++rejected_requests_;
            ScheduleResponse resp = make_base_response(req);
            resp.status = "rejected";
            return resp;
        }

        std::promise<RuntimeRunResult> promise;
        future = promise.get_future();
        pending_.emplace(*turn_id, std::move(promise));
    }

    cv_.notify_one();

    return make_completed_response(req, future.get());
}

RuntimeServiceSnapshot RuntimeService::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);

    RuntimeServiceSnapshot state;
    state.scheduler_policy =
        scheduler_policy_name(scheduler_config_.policy_kind);
    state.queued_turns = runtime_.queued_turn_count();
    state.inflight_backend_requests = inflight_backend_requests_;
    state.completed_requests = completed_requests_;
    state.rejected_requests = rejected_requests_;
    state.max_inflight_backend_requests =
        service_config_.max_inflight_backend_requests;
    state.max_runtime_queue_depth =
        service_config_.max_runtime_queue_depth;
    state.metrics = metrics_.summarize_all();
    return state;
}

void RuntimeService::dispatcher_loop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] {
            return stopping_ || can_dispatch_locked();
        });

        reap_finished_threads_locked();

        if (stopping_ &&
            runtime_.queued_turn_count() == 0 &&
            inflight_backend_requests_ == 0) {
            break;
        }

        dispatch_ready_turns_locked();
    }
}

bool RuntimeService::can_dispatch_locked() const {
    return runtime_.queued_turn_count() > 0 &&
           inflight_backend_requests_ <
               service_config_.max_inflight_backend_requests;
}

void RuntimeService::dispatch_ready_turns_locked() {
    while (can_dispatch_locked()) {
        auto admitted = runtime_.admit_next();
        if (!admitted.has_value()) {
            return;
        }

        ++inflight_backend_requests_;
        launch_backend_execution_locked(std::move(*admitted));
    }
}

void RuntimeService::launch_backend_execution_locked(
    RuntimeAdmittedTurn admitted
) {
    auto done = std::make_shared<std::atomic_bool>(false);

    ExecutionThread execution_thread;
    execution_thread.done = done;
    execution_thread.thread = std::thread(
        [this, admitted = std::move(admitted), done]() mutable {
            BackendResult backend_result =
                runtime_.execute_backend(admitted.turn.spec);

            finish_backend_execution(
                std::move(admitted),
                std::move(backend_result)
            );

            done->store(true);
        }
    );

    execution_threads_.push_back(std::move(execution_thread));
}

void RuntimeService::finish_backend_execution(
    RuntimeAdmittedTurn admitted,
    BackendResult backend_result
) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        runtime_.complete_turn(admitted.turn);

        RuntimeRunResult result;
        result.turn = std::move(admitted.turn);
        result.backend_result = std::move(backend_result);
        result.queue_wait_ms = admitted.queue_wait_ms;

        metrics_.record(
            result.turn,
            result.backend_result,
            result.queue_wait_ms
        );

        auto it = pending_.find(result.turn.turn_id);
        if (it != pending_.end()) {
            it->second.set_value(std::move(result));
            pending_.erase(it);
        }

        --inflight_backend_requests_;
        ++completed_requests_;
    }

    cv_.notify_all();
}

void RuntimeService::reap_finished_threads_locked() {
    for (auto it = execution_threads_.begin(); it != execution_threads_.end();) {
        if (it->done->load()) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = execution_threads_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ar
