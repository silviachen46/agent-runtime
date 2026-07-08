#include "agent_runtime/runtime_service.hpp"

#include <algorithm>
#include <chrono>
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

double bounded_multiplier(double value, double multiplier, double min_value, double max_value) {
    return std::clamp(value * multiplier, min_value, max_value);
}

int move_toward_int(int value, int target, int step) {
    if (value < target) {
        return std::min(value + step, target);
    }

    if (value > target) {
        return std::max(value - step, target);
    }

    return value;
}

double move_toward_double(double value, double target, double step_fraction) {
    return value + (target - value) * step_fraction;
}

int percentile(std::vector<int> values, double q) {
    if (values.empty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());
    const auto idx = static_cast<std::size_t>(
        static_cast<double>(values.size() - 1) * q
    );
    return values[idx];
}

bool is_focus_turn(const ReadyTurn& turn) {
    return turn.turn_type == TurnType::ResumeGenerate ||
           turn.session_policy.visibility == UserVisibility::Foreground ||
           turn.session_policy.priority > 0;
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
      baseline_scheduler_config_(scheduler_config),
      scheduler_config_(scheduler_config),
      runtime_(scheduler_config, std::move(backend)) {
    if (service_config_.max_inflight_backend_requests == 0) {
        service_config_.max_inflight_backend_requests = 1;
    }

    if (service_config_.max_runtime_queue_depth == 0) {
        service_config_.max_runtime_queue_depth = 1;
    }

    if (service_config_.admission_window_ms < 0) {
        service_config_.admission_window_ms = 0;
    }

    if (service_config_.max_admission_window_ms < 0) {
        service_config_.max_admission_window_ms = 0;
    }

    service_config_.admission_window_ms = std::min(
        service_config_.admission_window_ms,
        service_config_.max_admission_window_ms
    );

    if (service_config_.adaptive_window_size == 0) {
        service_config_.adaptive_window_size = 1;
    }

    if (service_config_.adaptive_latency_budget_ratio < 1.0) {
        service_config_.adaptive_latency_budget_ratio = 1.0;
    }

    if (service_config_.adaptive_latency_budget_ms < 0) {
        service_config_.adaptive_latency_budget_ms = 0;
    }

    if (service_config_.focus_queue_p95_target_ms < 0) {
        service_config_.focus_queue_p95_target_ms = 0;
    }

    if (service_config_.starvation_threshold_ms < 0) {
        service_config_.starvation_threshold_ms = 0;
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
    state.scheduler_config = scheduler_config_;
    state.queued_turns = runtime_.queued_turn_count();
    state.inflight_backend_requests = inflight_backend_requests_;
    state.completed_requests = completed_requests_;
    state.rejected_requests = rejected_requests_;
    state.max_inflight_backend_requests =
        service_config_.max_inflight_backend_requests;
    state.max_runtime_queue_depth =
        service_config_.max_runtime_queue_depth;
    state.admission_window_ms = service_config_.admission_window_ms;
    state.is_adaptive = service_config_.is_adaptive;
    state.adaptive_window_size = service_config_.adaptive_window_size;
    state.adaptive_updates = adaptive_updates_;
    state.adaptive_latency_budget_ratio =
        service_config_.adaptive_latency_budget_ratio;
    state.adaptive_latency_budget_ms =
        service_config_.adaptive_latency_budget_ms;
    state.adaptive_latency_baseline_p95_ms =
        adaptive_latency_baseline_p95_ms_;
    state.focus_queue_p95_target_ms =
        service_config_.focus_queue_p95_target_ms;
    state.starvation_threshold_ms =
        service_config_.starvation_threshold_ms;
    state.max_admission_window_ms =
        service_config_.max_admission_window_ms;
    state.metrics = metrics_.summarize_all();
    return state;
}

void RuntimeService::dispatcher_loop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] {
            return stopping_ || can_dispatch_locked();
        });

        if (!stopping_ && service_config_.admission_window_ms > 0) {
            cv_.wait_for(
                lock,
                std::chrono::milliseconds(
                    service_config_.admission_window_ms
                ),
                [this] {
                    return stopping_;
                }
            );
        }

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
        record_adaptive_feedback_locked(result);
        maybe_update_adaptive_policy_locked();

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

void RuntimeService::record_adaptive_feedback_locked(
    const RuntimeRunResult& result
) {
    if (!service_config_.is_adaptive) {
        return;
    }

    const int total_latency_ms =
        result.queue_wait_ms + result.backend_result.total_latency_ms;

    AdaptiveRecord record;
    record.focus = is_focus_turn(result.turn);
    record.deadline_missed =
        result.turn.slo.deadline_ms > 0 &&
        total_latency_ms > result.turn.slo.deadline_ms;
    record.queue_wait_ms = result.queue_wait_ms;
    record.total_latency_ms = total_latency_ms;

    adaptive_records_.push_back(record);
}

void RuntimeService::maybe_update_adaptive_policy_locked() {
    if (!service_config_.is_adaptive ||
        adaptive_records_.size() < service_config_.adaptive_window_size) {
        return;
    }

    std::vector<int> all_queue_waits;
    std::vector<int> focus_queue_waits;
    std::vector<int> all_latencies;
    std::size_t all_misses = 0;
    std::size_t focus_count = 0;
    std::size_t focus_misses = 0;

    all_queue_waits.reserve(adaptive_records_.size());
    all_latencies.reserve(adaptive_records_.size());

    for (const auto& record : adaptive_records_) {
        all_queue_waits.push_back(record.queue_wait_ms);
        all_latencies.push_back(record.total_latency_ms);

        if (record.deadline_missed) {
            ++all_misses;
        }

        if (record.focus) {
            ++focus_count;
            focus_queue_waits.push_back(record.queue_wait_ms);

            if (record.deadline_missed) {
                ++focus_misses;
            }
        }
    }

    const double all_miss_rate =
        static_cast<double>(all_misses) /
        static_cast<double>(adaptive_records_.size());
    const double focus_miss_rate = focus_count == 0
        ? 0.0
        : static_cast<double>(focus_misses) /
          static_cast<double>(focus_count);

    const int all_queue_p95 = percentile(all_queue_waits, 0.95);
    const int focus_queue_p95 = percentile(focus_queue_waits, 0.95);
    const int all_latency_p95 = percentile(all_latencies, 0.95);

    constexpr double kFocusMissTarget = 0.10;
    constexpr double kAllMissTarget = 0.20;
    if (adaptive_latency_baseline_p95_ms_ == 0 && all_latency_p95 > 0) {
        adaptive_latency_baseline_p95_ms_ = all_latency_p95;
    }

    const int learned_latency_budget_ms =
        adaptive_latency_baseline_p95_ms_ == 0
            ? 0
            : static_cast<int>(
                  static_cast<double>(adaptive_latency_baseline_p95_ms_) *
                  service_config_.adaptive_latency_budget_ratio
              );

    const int latency_budget_ms =
        service_config_.adaptive_latency_budget_ms > 0
            ? service_config_.adaptive_latency_budget_ms
            : learned_latency_budget_ms;

    const bool latency_budget_exceeded =
        latency_budget_ms > 0 && all_latency_p95 > latency_budget_ms;
    const bool focus_starving =
        service_config_.starvation_threshold_ms > 0 &&
        focus_queue_p95 >= service_config_.starvation_threshold_ms;
    const bool focus_above_target =
        service_config_.focus_queue_p95_target_ms > 0 &&
        focus_queue_p95 > service_config_.focus_queue_p95_target_ms;

    SchedulerConfig next = scheduler_config_;

    if (latency_budget_exceeded) {
        next.foreground_boost = move_toward_int(
            next.foreground_boost,
            baseline_scheduler_config_.foreground_boost,
            8
        );
        next.resume_turn_boost = move_toward_int(
            next.resume_turn_boost,
            baseline_scheduler_config_.resume_turn_boost,
            8
        );
        next.high_latency_boost = move_toward_int(
            next.high_latency_boost,
            baseline_scheduler_config_.high_latency_boost,
            6
        );
        next.latency_sensitive_boost = move_toward_int(
            next.latency_sensitive_boost,
            baseline_scheduler_config_.latency_sensitive_boost,
            4
        );
        next.deadline_urgency_weight = move_toward_double(
            next.deadline_urgency_weight,
            baseline_scheduler_config_.deadline_urgency_weight,
            0.15
        );
        next.token_cost_penalty = move_toward_double(
            next.token_cost_penalty,
            baseline_scheduler_config_.token_cost_penalty,
            0.20
        );
        next.aging_boost_per_ms = bounded_multiplier(
            next.aging_boost_per_ms,
            1.15,
            baseline_scheduler_config_.aging_boost_per_ms,
            0.50
        );
        service_config_.admission_window_ms =
            std::max(service_config_.admission_window_ms - 5, 0);
    } else if (focus_miss_rate > kFocusMissTarget || focus_above_target) {
        next.foreground_boost = std::min(next.foreground_boost + 8, 200);
        next.high_latency_boost = std::min(next.high_latency_boost + 5, 150);
        next.resume_turn_boost = std::min(next.resume_turn_boost + 8, 200);
        next.latency_sensitive_boost =
            std::min(next.latency_sensitive_boost + 4, 120);
        next.deadline_urgency_weight = bounded_multiplier(
            next.deadline_urgency_weight,
            1.15,
            baseline_scheduler_config_.deadline_urgency_weight,
            20000.0
        );
        next.token_cost_penalty = bounded_multiplier(
            next.token_cost_penalty,
            1.20,
            baseline_scheduler_config_.token_cost_penalty,
            8.0
        );

        service_config_.admission_window_ms =
            std::min(
                service_config_.admission_window_ms + 5,
                service_config_.max_admission_window_ms
            );
    }

    if (focus_starving) {
        next.foreground_boost = std::min(next.foreground_boost + 12, 240);
        next.resume_turn_boost = std::min(next.resume_turn_boost + 12, 240);
        next.deadline_urgency_weight = bounded_multiplier(
            next.deadline_urgency_weight,
            1.20,
            baseline_scheduler_config_.deadline_urgency_weight,
            25000.0
        );
        next.aging_boost_per_ms = bounded_multiplier(
            next.aging_boost_per_ms,
            1.35,
            baseline_scheduler_config_.aging_boost_per_ms,
            0.75
        );
        next.token_cost_penalty = std::max(next.token_cost_penalty * 0.85, 0.0);
        service_config_.admission_window_ms = std::min(
            service_config_.admission_window_ms,
            service_config_.max_admission_window_ms
        );
    }

    if (all_miss_rate > kAllMissTarget || all_queue_p95 > 5000) {
        next.aging_boost_per_ms = bounded_multiplier(
            next.aging_boost_per_ms,
            1.20,
            baseline_scheduler_config_.aging_boost_per_ms,
            0.20
        );
        next.token_cost_penalty = bounded_multiplier(
            next.token_cost_penalty,
            1.10,
            baseline_scheduler_config_.token_cost_penalty,
            10.0
        );

        if (focus_miss_rate <= kFocusMissTarget) {
            next.foreground_boost = move_toward_int(
                next.foreground_boost,
                baseline_scheduler_config_.foreground_boost,
                5
            );
            next.high_latency_boost = move_toward_int(
                next.high_latency_boost,
                baseline_scheduler_config_.high_latency_boost,
                5
            );
            next.deadline_urgency_weight = move_toward_double(
                next.deadline_urgency_weight,
                baseline_scheduler_config_.deadline_urgency_weight,
                0.10
            );
        }
    }

    if (!latency_budget_exceeded &&
        !focus_starving &&
        focus_miss_rate <= kFocusMissTarget &&
        all_miss_rate <= kAllMissTarget &&
        all_queue_p95 < 1000 &&
        focus_queue_p95 < 1000 &&
        all_latency_p95 > 0) {
        next.foreground_boost = move_toward_int(
            next.foreground_boost,
            baseline_scheduler_config_.foreground_boost,
            2
        );
        next.high_latency_boost = move_toward_int(
            next.high_latency_boost,
            baseline_scheduler_config_.high_latency_boost,
            2
        );
        next.deadline_urgency_weight = move_toward_double(
            next.deadline_urgency_weight,
            baseline_scheduler_config_.deadline_urgency_weight,
            0.05
        );
        next.aging_boost_per_ms = move_toward_double(
            next.aging_boost_per_ms,
            baseline_scheduler_config_.aging_boost_per_ms,
            0.05
        );
        next.token_cost_penalty = move_toward_double(
            next.token_cost_penalty,
            baseline_scheduler_config_.token_cost_penalty,
            0.05
        );
        service_config_.admission_window_ms =
            std::max(service_config_.admission_window_ms - 1, 0);
    }

    service_config_.admission_window_ms = std::clamp(
        service_config_.admission_window_ms,
        0,
        service_config_.max_admission_window_ms
    );

    scheduler_config_ = next;
    runtime_.update_scheduler_config(scheduler_config_);
    adaptive_records_.clear();
    ++adaptive_updates_;
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
