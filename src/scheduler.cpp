#include "agent_runtime/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace ar {

namespace {

int64_t wait_ms_for(const ReadyTurn& turn, TimePoint now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - turn.enqueued_at
    ).count();
}

double deadline_urgency(const ReadyTurn& turn, TimePoint now) {
    if (turn.slo.deadline_ms <= 0) {
        return 0.0;
    }

    const int64_t remaining_ms = std::max<int64_t>(
        static_cast<int64_t>(turn.slo.deadline_ms) - wait_ms_for(turn, now),
        1
    );

    return 1.0 / static_cast<double>(remaining_ms);
}

} // namespace

std::string scheduler_policy_name(SchedulerPolicyKind policy_kind) {
    switch (policy_kind) {
        case SchedulerPolicyKind::Fifo:
            return "fifo";
        case SchedulerPolicyKind::Priority:
            return "priority";
        case SchedulerPolicyKind::SloAware:
            return "slo_aware";
        case SchedulerPolicyKind::SessionAwareHybrid:
            return "session_aware_hybrid";
    }

    return "unknown";
}

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

void Scheduler::update_config(const SchedulerConfig& config) {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = config;
}

SchedulerConfig Scheduler::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

bool Scheduler::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ready_queue_.empty();
}

std::size_t Scheduler::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ready_queue_.size();
}


double Scheduler::score_turn(const ReadyTurn& turn, TimePoint now) const {
    if (config_.policy_kind == SchedulerPolicyKind::Priority) {
        return static_cast<double>(turn.session_policy.priority);
    }

    if (config_.policy_kind == SchedulerPolicyKind::SloAware) {
        return config_.deadline_urgency_weight * deadline_urgency(turn, now);
    }

    double score = turn.scheduling_policy.effective_priority;

    score += config_.deadline_urgency_weight * deadline_urgency(turn, now);

    if (turn.turn_type == TurnType::ResumeGenerate) {
        score += config_.resume_turn_boost;
    }

    if (turn.scheduling_policy.latency_sensitive) {
        score += config_.latency_sensitive_boost;
    }

    const auto wait_ms = wait_ms_for(turn, now);

    score += static_cast<double>(wait_ms) * config_.aging_boost_per_ms;
    score -= static_cast<double>(turn.spec.max_tokens) * config_.token_cost_penalty;

    return score;
}

bool Scheduler::is_better(
    const ReadyTurn& candidate,
    const ReadyTurn& current_best,
    TimePoint now
) const {
    if (config_.policy_kind == SchedulerPolicyKind::Fifo) {
        return candidate.enqueued_at < current_best.enqueued_at;
    }

    const double candidate_score = score_turn(candidate, now);
    const double best_score = score_turn(current_best, now);

    if (candidate_score == best_score) {
        return candidate.enqueued_at < current_best.enqueued_at;
    }

    return candidate_score > best_score;
}

std::optional<ReadyTurn> Scheduler::pick_next() {
    std::lock_guard<std::mutex> lock(mu_);

    if (ready_queue_.empty()) {
        return std::nullopt;
    }

    const TimePoint now = std::chrono::steady_clock::now();

    std::size_t best_idx = 0;

    for (std::size_t i = 1; i < ready_queue_.size(); ++i) {
        if (is_better(ready_queue_[i], ready_queue_[best_idx], now)) {
            best_idx = i;
        }
    }

    ReadyTurn selected = std::move(ready_queue_[best_idx]);
    ready_queue_.erase(ready_queue_.begin() + static_cast<std::ptrdiff_t>(best_idx));

    return selected;
}

} // namespace ar
