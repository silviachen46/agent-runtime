#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ar {

enum class SchedulerPolicyKind {
    Fifo,
    Priority,
    PriorityFair,
    PriorityTailAging,
    SloAware,
    SessionAwareHybrid
};

struct SchedulerConfig {
    SchedulerPolicyKind policy_kind = SchedulerPolicyKind::SessionAwareHybrid;

    int foreground_boost = 50;
    int high_latency_boost = 30;
    int medium_latency_boost = 10;
    int low_latency_penalty = -10;

    int resume_turn_boost = 50;
    int latency_sensitive_boost = 20;

    double deadline_urgency_weight = 1000.0;
    double aging_boost_per_ms = 0.01;
    double token_cost_penalty = 0.01;
    int tail_aging_threshold_ms = 12000;
    double tail_aging_boost_per_ms = 0.002;
};

std::string scheduler_policy_name(SchedulerPolicyKind policy_kind);

SchedulingPolicy make_scheduling_policy(
    const SessionPolicy& policy,
    const SchedulerConfig& config = SchedulerConfig{}
);

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig config = SchedulerConfig{});

    void enqueue(ReadyTurn turn);
    std::optional<ReadyTurn> pick_next();
    std::optional<ReadyTurn> pick_next_matching(
        const std::function<bool(const ReadyTurn&)>& predicate
    );

    void update_config(const SchedulerConfig& config);
    SchedulerConfig config() const;

    bool empty() const;
    std::size_t size() const;
    std::size_t count_matching(
        const std::function<bool(const ReadyTurn&)>& predicate
    ) const;

private:
    double score_turn(const ReadyTurn& turn, TimePoint now) const;
    bool is_better(
        const ReadyTurn& candidate,
        const ReadyTurn& current_best,
        TimePoint now
    ) const;

    SchedulerConfig config_;

    mutable std::mutex mu_;
    std::vector<ReadyTurn> ready_queue_;
};

} // namespace ar
