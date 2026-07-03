#include "agent_runtime/metrics.hpp"

#include <algorithm>
#include <numeric>

namespace ar {

namespace {

double average(const std::vector<int>& values) {
    if (values.empty()) {
        return 0.0;
    }

    const int sum = std::accumulate(values.begin(), values.end(), 0);
    return static_cast<double>(sum) / static_cast<double>(values.size());
}

double percentile(std::vector<int> values, double q) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    const auto last_idx = values.size() - 1;
    const auto idx = static_cast<std::size_t>(
        static_cast<double>(last_idx) * q
    );

    return static_cast<double>(values[idx]);
}

} // namespace

void MetricsCollector::record(
    const ReadyTurn& turn,
    const BackendResult& backend_result,
    int queue_wait_ms
) {
    TurnRecord record;
    record.turn_id = turn.turn_id;
    record.session_id = turn.session_id;
    record.turn_type = turn.turn_type;
    record.queue_wait_ms = queue_wait_ms;
    record.ttft_ms = backend_result.ttft_ms;
    record.total_latency_ms = backend_result.total_latency_ms;
    record.output_tokens = backend_result.output_tokens;

    records_.push_back(std::move(record));
}

MetricsSummary MetricsCollector::summarize_all() const {
    return summarize_records(records_);
}

MetricsSummary MetricsCollector::summarize_turn_type(TurnType turn_type) const {
    std::vector<TurnRecord> filtered;

    for (const auto& record : records_) {
        if (record.turn_type == turn_type) {
            filtered.push_back(record);
        }
    }

    return summarize_records(filtered);
}

const std::vector<TurnRecord>& MetricsCollector::records() const {
    return records_;
}

MetricsSummary MetricsCollector::summarize_records(
    const std::vector<TurnRecord>& records
) const {
    MetricsSummary summary;
    summary.count = records.size();

    if (records.empty()) {
        return summary;
    }

    std::vector<int> queue_waits;
    std::vector<int> ttfts;
    std::vector<int> total_latencies;
    std::vector<int> output_tokens;

    queue_waits.reserve(records.size());
    ttfts.reserve(records.size());
    total_latencies.reserve(records.size());
    output_tokens.reserve(records.size());

    for (const auto& record : records) {
        queue_waits.push_back(record.queue_wait_ms);
        ttfts.push_back(record.ttft_ms);
        total_latencies.push_back(record.total_latency_ms);
        output_tokens.push_back(record.output_tokens);
    }

    summary.avg_queue_wait_ms = average(queue_waits);
    summary.p50_queue_wait_ms = percentile(queue_waits, 0.50);
    summary.p95_queue_wait_ms = percentile(queue_waits, 0.95);
    summary.p99_queue_wait_ms = percentile(queue_waits, 0.99);

    summary.avg_ttft_ms = average(ttfts);
    summary.p50_ttft_ms = percentile(ttfts, 0.50);
    summary.p95_ttft_ms = percentile(ttfts, 0.95);
    summary.p99_ttft_ms = percentile(ttfts, 0.99);

    summary.avg_total_latency_ms = average(total_latencies);
    summary.p50_total_latency_ms = percentile(total_latencies, 0.50);
    summary.p95_total_latency_ms = percentile(total_latencies, 0.95);
    summary.p99_total_latency_ms = percentile(total_latencies, 0.99);

    summary.avg_output_tokens = average(output_tokens);

    return summary;
}

} // namespace ar