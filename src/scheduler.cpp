#include "agent_runtime/scheduler.hpp"

#include <chrono>
#include <utility>

namespace ar {
    SchedulingPolicy make_scheduling_policy(
    const SessionPolicy& policy,
    const SchedulerConfig& config
){
    SchedulingPolicy result;
    result.effective_priority = policy.priority;
    if(policy.visibility == UserVisibility::Foreground){
        result.preemptible = false;
        result.effective_priority += config.foreground_boost;
    }else{
        result.preemptible = true;
    }

    if (policy.latency == LatencySensitivity::High) {
        result.effective_priority += config.high_latency_boost;
        result.latency_sensitive = true;
    } else if (policy.latency == LatencySensitivity::Medium) {
        result.effective_priority += config.medium_latency_boost;
        result.latency_sensitive = true;
    } else {
        result.effective_priority += config.low_latency_penalty;
        result.latency_sensitive = false;
    }
    if (policy.workload == WorkloadKind::Agent) {
        result.weight = 3;
    } else if (policy.workload == WorkloadKind::Chat) {
        result.weight = 2;
    } else {
        result.weight = 1;
    }

    return result;
}

Scheduler::Scheduler(SchedulerConfig config)
    : config_(config) {}

    void Scheduler::enqueue(ReadyTurn turn) {
    std::lock_guard<std::mutex> lock(mu_);
    ready_queue_.push_back(std::move(turn));
}

bool Scheduler::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ready_queue_.empty();
}

std::size_t Scheduler::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ready_queue_.size();
}

std::size_t Scheduler::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ready_queue_.size();
}

double Scheduler::score_turn(const ReadyTurn& turn, TimePoint now) const {
    double score = turn.scheduling_policy.effective_priority;

    if (turn.turn_type == TurnType::ResumeGenerate) {
        score += config_.resume_turn_boost;
    }

    if (turn.scheduling_policy.latency_sensitive) {
        score += config_.latency_sensitive_boost;
    }

    const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - turn.enqueued_at
    ).count();

    score += static_cast<double>(wait_ms) * config_.aging_boost_per_ms;

    return score;
}

std::optional<ReadyTurn> Scheduler::pick_next() {
    std::lock_guard<std::mutex> lock(mu_);

    if (ready_queue_.empty()) {
        return std::nullopt;
    }

    const TimePoint now = std::chrono::steady_clock::now();

    std::size_t best_idx = 0;
    double best_score = score_turn(ready_queue_[0], now);

    for (std::size_t i = 1; i < ready_queue_.size(); ++i) {
        const double current_score = score_turn(ready_queue_[i], now);
        if (current_score > best_score) {
            best_score = current_score;
            best_idx = i;
        }
    }

    ReadyTurn selected = std::move(ready_queue_[best_idx]);
    ready_queue_.erase(ready_queue_.begin() + static_cast<std::ptrdiff_t>(best_idx));

    return selected;
}
};

