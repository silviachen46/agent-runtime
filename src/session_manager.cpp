#include "agent_runtime/session_manager.hpp"

#include <chrono>
#include <utility>

namespace ar {
    bool SessionManager::create_session(const SessionSpec& spec){
        std::lock_guard<std::mutex> lock(mu_);
        if (sessions_.contains(spec.session_id)) {
        return false;
    }
        const TimePoint now = std::chrono::steady_clock::now();
        SessionState state;
        state.session_id = spec.session_id;
        state.policy = spec.policy;
        state.status = SessionStatus::Ready;
        state.slo = spec.slo;
        state.created_at = now;
        state.last_active_at = now;

        sessions_.emplace(spec.session_id, std::move(state));
        return true;

    }
    std::optional<SessionState> SessionManager::get(const std::string& session_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool SessionManager::update_status_locked(
    const std::string& session_id,
    SessionStatus status,
    std::optional<ToolWaitSpec> tool_wait
) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    it->second.status = status;
    it->second.last_active_at = std::chrono::steady_clock::now();
    it->second.tool_wait = std::move(tool_wait);

    return true;
}
bool SessionManager::mark_running(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    return update_status_locked(session_id, SessionStatus::Running, std::nullopt);
}

bool SessionManager::mark_ready(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    return update_status_locked(session_id, SessionStatus::Ready, std::nullopt);
}

bool SessionManager::mark_tool_waiting(
    const std::string& session_id,
    const ToolWaitSpec& wait
) {
    std::lock_guard<std::mutex> lock(mu_);
    return update_status_locked(session_id, SessionStatus::ToolWaiting, wait);
}

bool SessionManager::mark_finished(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    return update_status_locked(session_id, SessionStatus::Finished, std::nullopt);
}

bool SessionManager::cancel(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    return update_status_locked(session_id, SessionStatus::Cancelled, std::nullopt);
}
}