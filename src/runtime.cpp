#include "agent_runtime/runtime.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace ar {

namespace {

bool is_focus_turn(const ReadyTurn& turn) {
    return turn.turn_type == TurnType::ResumeGenerate ||
           turn.session_policy.priority > 0;
}

} // namespace

Runtime::Runtime(
    SchedulerConfig scheduler_config,
    MockBackendConfig backend_config
)
    : Runtime(
          scheduler_config,
          make_mock_backend(backend_config)
      ) {}

Runtime::Runtime(
    SchedulerConfig scheduler_config,
    std::shared_ptr<Backend> backend
)
    : scheduler_config_(scheduler_config),
      scheduler_(scheduler_config_),
      backend_(std::move(backend)) {}

bool Runtime::create_session(const SessionSpec& spec) {
    return session_manager_.create_session(spec);
}

bool Runtime::submit_turn(const TurnSpec& spec) {
    return submit_turn_with_id(spec).has_value();
}

std::optional<std::string> Runtime::submit_turn_with_id(const TurnSpec& spec) {
    auto session = session_manager_.get(spec.session_id);
    if (!session.has_value()) {
        return std::nullopt;
    }

    if (session->status == SessionStatus::Finished ||
        session->status == SessionStatus::Cancelled) {
        return std::nullopt;
    }

    const TimePoint now = std::chrono::steady_clock::now();

    ReadyTurn ready;
    ready.turn_id = next_turn_id();
    ready.session_id = spec.session_id;
    ready.session_policy = session->policy;
    ready.scheduling_policy = make_scheduling_policy(
    session->policy,
    scheduler_config_
);
    ready.slo = session->slo;
    ready.turn_type = spec.turn_type;
    ready.enqueued_at = now;
    ready.spec = spec;

    const std::string turn_id = ready.turn_id;
    scheduler_.enqueue(std::move(ready));
    session_manager_.mark_ready(spec.session_id);

    return turn_id;
}

std::optional<RuntimeAdmittedTurn> Runtime::admit_next() {
    return admit_next_matching([](const ReadyTurn&) {
        return true;
    });
}

std::optional<RuntimeAdmittedTurn> Runtime::admit_next_matching(
    const std::function<bool(const ReadyTurn&)>& predicate
) {
    auto ready = scheduler_.pick_next_matching(predicate);
    if (!ready.has_value()) {
        return std::nullopt;
    }

    const TimePoint started_at = std::chrono::steady_clock::now();

    const auto queue_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        started_at - ready->enqueued_at
    ).count();

    session_manager_.mark_running(ready->session_id);

    RuntimeAdmittedTurn admitted;
    admitted.turn = std::move(*ready);
    admitted.queue_wait_ms = static_cast<int>(queue_wait_ms);

    return admitted;
}

BackendResult Runtime::execute_backend(const TurnSpec& spec) const {
    return backend_->run(spec);
}

void Runtime::complete_turn(const ReadyTurn& turn) {
    session_manager_.mark_ready(turn.session_id);
}

std::optional<RuntimeRunResult> Runtime::run_once() {
    auto admitted = admit_next();
    if (!admitted.has_value()) {
        return std::nullopt;
    }

    BackendResult backend_result = execute_backend(admitted->turn.spec);
    complete_turn(admitted->turn);

    RuntimeRunResult result;
    result.turn = std::move(admitted->turn);
    result.backend_result = std::move(backend_result);
    result.queue_wait_ms = admitted->queue_wait_ms;

    return result;
}

int Runtime::run_until_idle() {
    int executed = 0;

    while (true) {
        auto result = run_once();
        if (!result.has_value()) {
            break;
        }

        ++executed;
    }

    return executed;
}

void Runtime::update_scheduler_config(const SchedulerConfig& config) {
    scheduler_config_ = config;
    scheduler_.update_config(config);
}

SchedulerConfig Runtime::scheduler_config() const {
    return scheduler_config_;
}

std::size_t Runtime::queued_turn_count() const {
    return scheduler_.size();
}

std::size_t Runtime::queued_focus_turn_count() const {
    return scheduler_.count_matching(is_focus_turn);
}

std::optional<SessionState> Runtime::get_session(
    const std::string& session_id
) const {
    return session_manager_.get(session_id);
}

std::string Runtime::next_turn_id() {
    return "turn_" + std::to_string(next_turn_seq_++);
}

} // namespace ar
