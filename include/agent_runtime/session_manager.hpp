#pragma once

#include "types.hpp"
#include <mutex>
#include <string>
#include <unordered_map>
#include <optional>

namespace ar {

    struct SessionState {
    std::string session_id;

    SessionPolicy policy;
    SessionStatus status = SessionStatus::Ready;
    SLO slo;

    TimePoint created_at;
    TimePoint last_active_at;

    std::optional<ToolWaitSpec> tool_wait;
};

class SessionManager {
public:
    bool create_session(const SessionSpec& spec);

    bool mark_running(const std::string& session_id);
    bool mark_ready(const std::string& session_id);
    bool mark_tool_waiting(const std::string& session_id, const ToolWaitSpec& wait);
    bool mark_finished(const std::string& session_id);
    bool cancel(const std::string& session_id);

    std::optional<SessionState> get(const std::string& session_id) const;

private:
    bool update_status_locked(
        const std::string& session_id,
        SessionStatus status,
        std::optional<ToolWaitSpec> tool_wait
    );

    mutable std::mutex mu_;
    std::unordered_map<std::string, SessionState> sessions_;
};

}