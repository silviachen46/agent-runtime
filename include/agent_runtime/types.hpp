#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace ar {
    using TimePoint = std::chrono::steady_clock::time_point;
    enum class UserVisibility {
        Foreground,
        Background
    };

    enum class WorkloadKind {
        Chat,
        Agent,
        Batch
    };

    enum class LatencySensitivity {
        High,
        Medium,
        Low
    };

    struct SessionPolicy {
        UserVisibility visibility = UserVisibility::Foreground;
        WorkloadKind workload = WorkloadKind::Agent;
        LatencySensitivity latency = LatencySensitivity::Medium;
        int priority = 0;
    };

    struct SchedulingPolicy {
        int effective_priority = 0;
        int weight = 1;
        bool preemptible = true;
        bool latency_sensitive = true;
    };

    enum class SessionStatus {
        Ready,
        Running,
        ToolWaiting,
        Finished,
        Cancelled
    };
    enum class TurnType {
        InitialGenerate,
        ResumeGenerate,
        BackgroundGenerate
    };

    struct SLO {
        int ttft_target_ms = 1000;
        int resume_target_ms = 1000;
        int deadline_ms = 0;
    };

    struct Message {
        std::string role;
        std::string content;
    };

    struct SessionSpec {
        std::string session_id;
        SessionPolicy policy;
        SLO slo;
    };

    struct ToolWaitSpec {
        std::string tool_name;
        int expected_wait_ms = 0;
    };

    struct TurnSpec {
        std::string session_id;
        TurnType turn_type;
        std::vector<Message> messages;
        int max_tokens = 512;
        double temperature = 0.2;
        bool stream = true;
    };
    
    struct ReadyTurn {
        std::string turn_id;
        std::string session_id;

        SessionPolicy session_policy;
        SchedulingPolicy scheduling_policy;
        SLO slo;

        TurnType turn_type;
        TimePoint enqueued_at;
        TurnSpec spec;
    };

    struct TokenEvent {
        std::string text;
        bool is_first_token = false;
        bool done = false;
    };

    struct ScheduledRequest {
    std::string request_id;
    std::string session_id;
    std::string workflow_id;
    std::string step_id;

    int priority = 0;
    int deadline_ms = 0;
    int ttft_target_ms = 0;
    int resume_target_ms = 0;

    std::string prompt;
    int max_tokens = 256;
    double temperature = 0.2;

    bool is_resume = false;

    long long arrival_ts_ms = 0;
};

struct ScheduleResponse {
    std::string request_id;
    std::string session_id;
    std::string workflow_id;
    std::string step_id;

    std::string status;
    std::string output;

    int queue_wait_ms = 0;
    int ttft_ms = 0;
    int total_latency_ms = 0;
    int output_tokens = 0;

    bool deadline_missed = false;
};
}