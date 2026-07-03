#include "agent_runtime/backend.hpp"
#include "agent_runtime/metrics.hpp"
#include "agent_runtime/runtime.hpp"
#include "agent_runtime/scheduler.hpp"
#include "agent_runtime/types.hpp"

#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace ar;

constexpr int kForegroundSessions = 20;
constexpr int kBackgroundBeforeEachResume = 5;
constexpr int kBackgroundTail = 20;

SessionSpec make_foreground_agent_session(const std::string& session_id) {
    SessionSpec spec;
    spec.session_id = session_id;
    spec.policy.visibility = UserVisibility::Foreground;
    spec.policy.workload = WorkloadKind::Agent;
    spec.policy.latency = LatencySensitivity::High;
    spec.policy.priority = 0;
    spec.slo.ttft_target_ms = 800;
    spec.slo.resume_target_ms = 300;
    return spec;
}

SessionSpec make_background_batch_session(const std::string& session_id) {
    SessionSpec spec;
    spec.session_id = session_id;
    spec.policy.visibility = UserVisibility::Background;
    spec.policy.workload = WorkloadKind::Batch;
    spec.policy.latency = LatencySensitivity::Low;
    spec.policy.priority = 0;
    spec.slo.ttft_target_ms = 3000;
    spec.slo.resume_target_ms = 3000;
    return spec;
}

TurnSpec make_background_turn(const std::string& session_id) {
    TurnSpec spec;
    spec.session_id = session_id;
    spec.turn_type = TurnType::BackgroundGenerate;
    spec.max_tokens = 8;
    spec.temperature = 0.2;
    spec.stream = true;
    spec.messages.push_back(Message{
        .role = "user",
        .content = "Run background batch work."
    });
    return spec;
}

TurnSpec make_resume_turn(const std::string& session_id) {
    TurnSpec spec;
    spec.session_id = session_id;
    spec.turn_type = TurnType::ResumeGenerate;
    spec.max_tokens = 8;
    spec.temperature = 0.2;
    spec.stream = true;
    spec.messages.push_back(Message{
        .role = "tool",
        .content = "Tool result is ready. Resume generation."
    });
    return spec;
}

struct BenchmarkResult {
    MetricsSummary all;
    MetricsSummary resume;
};

BenchmarkResult run_mixed_workload(
    const SchedulerConfig& scheduler_config,
    const MockBackendConfig& backend_config
) {
    Runtime runtime{
        scheduler_config,
        backend_config
    };

    MetricsCollector metrics;

    int background_id = 0;

    for (int agent_id = 0; agent_id < kForegroundSessions; ++agent_id) {
        for (int j = 0; j < kBackgroundBeforeEachResume; ++j) {
            const std::string session_id =
                "batch_before_" + std::to_string(background_id++);

            runtime.create_session(make_background_batch_session(session_id));
            runtime.submit_turn(make_background_turn(session_id));
        }

        const std::string agent_session_id =
            "foreground_agent_" + std::to_string(agent_id);

        runtime.create_session(make_foreground_agent_session(agent_session_id));
        runtime.submit_turn(make_resume_turn(agent_session_id));
    }

    for (int i = 0; i < kBackgroundTail; ++i) {
        const std::string session_id =
            "batch_tail_" + std::to_string(background_id++);

        runtime.create_session(make_background_batch_session(session_id));
        runtime.submit_turn(make_background_turn(session_id));
    }

    while (true) {
        std::optional<RuntimeRunResult> result = runtime.run_once();
        if (!result.has_value()) {
            break;
        }

        metrics.record(
            result->turn,
            result->backend_result,
            result->queue_wait_ms
        );
    }

    BenchmarkResult result;
    result.all = metrics.summarize_all();
    result.resume = metrics.summarize_turn_type(TurnType::ResumeGenerate);
    return result;
}

void print_summary(const std::string& name, const BenchmarkResult& result) {
    std::cout << "== " << name << " ==\n";

    std::cout << "all.count: " << result.all.count << "\n";
    std::cout << "all.avg_queue_wait_ms: "
              << result.all.avg_queue_wait_ms << "\n";
    std::cout << "all.p95_queue_wait_ms: "
              << result.all.p95_queue_wait_ms << "\n";

    std::cout << "resume.count: " << result.resume.count << "\n";
    std::cout << "resume.avg_queue_wait_ms: "
              << result.resume.avg_queue_wait_ms << "\n";
    std::cout << "resume.p50_queue_wait_ms: "
              << result.resume.p50_queue_wait_ms << "\n";
    std::cout << "resume.p95_queue_wait_ms: "
              << result.resume.p95_queue_wait_ms << "\n";
    std::cout << "resume.p99_queue_wait_ms: "
              << result.resume.p99_queue_wait_ms << "\n";

    std::cout << "\n";
}

double improvement_percent(double baseline, double improved) {
    if (baseline <= 0.0) {
        return 0.0;
    }

    return (baseline - improved) * 100.0 / baseline;
}

} // namespace

int main() {
    SchedulerConfig fifo_config;
    fifo_config.foreground_boost = 0;
    fifo_config.high_latency_boost = 0;
    fifo_config.medium_latency_boost = 0;
    fifo_config.low_latency_penalty = 0;
    fifo_config.resume_turn_boost = 0;
    fifo_config.latency_sensitive_boost = 0;
    fifo_config.aging_boost_per_ms = 0.0;

    SchedulerConfig session_aware_config;

    MockBackendConfig backend_config;
    backend_config.initial_ttft_ms = 10;
    backend_config.resume_ttft_ms = 5;
    backend_config.background_ttft_ms = 10;
    backend_config.per_token_ms = 1;
    backend_config.default_output_tokens = 8;
    backend_config.sleep_enabled = true;

    const BenchmarkResult fifo = run_mixed_workload(
        fifo_config,
        backend_config
    );

    const BenchmarkResult session_aware = run_mixed_workload(
        session_aware_config,
        backend_config
    );

    print_summary("FIFO baseline", fifo);
    print_summary("Session-aware scheduler", session_aware);

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "resume avg queue wait improvement: "
              << improvement_percent(
                     fifo.resume.avg_queue_wait_ms,
                     session_aware.resume.avg_queue_wait_ms
                 )
              << "%\n";

    std::cout << "resume p95 queue wait improvement: "
              << improvement_percent(
                     fifo.resume.p95_queue_wait_ms,
                     session_aware.resume.p95_queue_wait_ms
                 )
              << "%\n";

    std::cout << "resume p99 queue wait improvement: "
              << improvement_percent(
                     fifo.resume.p99_queue_wait_ms,
                     session_aware.resume.p99_queue_wait_ms
                 )
              << "%\n";

    return 0;
}