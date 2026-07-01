#pragma once

#include "types.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace ar {

struct SchedulerConfig {
    int foreground_boost = 50;
    int high_latency_boost = 30;
    int medium_latency_boost = 10;
    int low_latency_penalty = -10;

    int resume_turn_boost = 50;
    int latency_sensitive_boost = 20;

    double aging_boost_per_ms = 0.01;
};

SchedulingPolicy make_scheduling_policy(
    const SessionPolicy& policy,
    const SchedulerConfig& config = SchedulerConfig{}
);

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig config = SchedulerConfig{});

    void enqueue(ReadyTurn turn);
    std::optional<ReadyTurn> pick_next();

    bool empty() const;
    std::size_t size() const;

private:
    double score_turn(const ReadyTurn& turn, TimePoint now) const;

    SchedulerConfig config_;

    mutable std::mutex mu_;
    std::vector<ReadyTurn> ready_queue_;
};

} // namespace ar