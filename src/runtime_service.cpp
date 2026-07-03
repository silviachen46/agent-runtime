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
    SchedulerConfig scheduler_config,
    MockBackendConfig backend_config
)
    : runtime_(scheduler_config, backend_config),
      worker_(&RuntimeService::worker_loop, this) {}

RuntimeService::~RuntimeService() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

ScheduleResponse RuntimeService::schedule(const ScheduledRequest& req) {
    std::future<RuntimeRunResult> future;

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (!runtime_.get_session(req.session_id).has_value()) {
            runtime_.create_session(make_session_spec(req));
        }

        const auto turn_id = runtime_.submit_turn_with_id(make_turn_spec(req));
        if (!turn_id.has_value()) {
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

void RuntimeService::worker_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] {
                return stopping_ || runtime_.queued_turn_count() > 0;
            });

            if (stopping_ && runtime_.queued_turn_count() == 0) {
                break;
            }
        }

        auto result = runtime_.run_once();
        if (!result.has_value()) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(result->turn.turn_id);
        if (it != pending_.end()) {
            it->second.set_value(std::move(*result));
            pending_.erase(it);
        }
    }
}

} // namespace ar
