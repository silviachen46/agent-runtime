#pragma once

#include "backend.hpp"
#include "types.hpp"

#include <cstddef>
#include <vector>

namespace ar {

struct TurnRecord {
    std::string turn_id;
    std::string session_id;
    TurnType turn_type = TurnType::InitialGenerate;

    int queue_wait_ms = 0;
    int ttft_ms = 0;
    int total_latency_ms = 0;
    int output_tokens = 0;
    bool deadline_missed = false;
};

struct MetricsSummary {
    std::size_t count = 0;

    double avg_queue_wait_ms = 0.0;
    double p50_queue_wait_ms = 0.0;
    double p95_queue_wait_ms = 0.0;
    double p99_queue_wait_ms = 0.0;

    double avg_ttft_ms = 0.0;
    double p50_ttft_ms = 0.0;
    double p95_ttft_ms = 0.0;
    double p99_ttft_ms = 0.0;

    double avg_total_latency_ms = 0.0;
    double p50_total_latency_ms = 0.0;
    double p95_total_latency_ms = 0.0;
    double p99_total_latency_ms = 0.0;

    double avg_output_tokens = 0.0;

    std::size_t deadline_missed_count = 0;
    double deadline_miss_rate = 0.0;
};

class MetricsCollector {
public:
    void record(
        const ReadyTurn& turn,
        const BackendResult& backend_result,
        int queue_wait_ms
    );

    MetricsSummary summarize_all() const;
    MetricsSummary summarize_turn_type(TurnType turn_type) const;

    const std::vector<TurnRecord>& records() const;

private:
    MetricsSummary summarize_records(const std::vector<TurnRecord>& records) const;

    std::vector<TurnRecord> records_;
};

} // namespace ar
