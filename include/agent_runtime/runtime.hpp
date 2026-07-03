#pragma once

#include "backend.hpp"
#include "scheduler.hpp"
#include "session_manager.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ar {

struct RuntimeRunResult {
    ReadyTurn turn;
    BackendResult backend_result;

    int queue_wait_ms = 0;
};

class Runtime {
public:
    Runtime(
        SchedulerConfig scheduler_config = SchedulerConfig{},
        MockBackendConfig backend_config = MockBackendConfig{}
    );

    bool create_session(const SessionSpec& spec);
    bool submit_turn(const TurnSpec& spec);
    std::optional<std::string> submit_turn_with_id(const TurnSpec& spec);

    std::optional<RuntimeRunResult> run_once();
    int run_until_idle();

    std::size_t queued_turn_count() const;
    std::optional<SessionState> get_session(const std::string& session_id) const;

private:
    std::string next_turn_id();

    SessionManager session_manager_;
    SchedulerConfig scheduler_config_;
    Scheduler scheduler_;
    MockBackend backend_;

    std::uint64_t next_turn_seq_ = 1;
};

} // namespace ar
