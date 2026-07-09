#pragma once

#include "backend.hpp"
#include "metrics.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"
#include "types.hpp"

#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ar {

struct RuntimeServiceConfig {
    std::size_t max_inflight_backend_requests = 8;
    std::size_t max_runtime_queue_depth = 1000;
    std::size_t reserved_focus_slots = 0;
    int admission_window_ms = 0;
    bool is_adaptive = false;
    std::size_t adaptive_window_size = 50;
    double adaptive_latency_budget_ratio = 1.10;
    int adaptive_latency_budget_ms = 0;
    int focus_queue_p95_target_ms = 3000;
    int starvation_threshold_ms = 5000;
    int max_admission_window_ms = 30;
};

struct RuntimeServiceSnapshot {
    std::string scheduler_policy;
    SchedulerConfig scheduler_config;
    std::size_t queued_turns = 0;
    std::size_t inflight_backend_requests = 0;
    std::size_t completed_requests = 0;
    std::size_t rejected_requests = 0;
    std::size_t max_inflight_backend_requests = 0;
    std::size_t max_runtime_queue_depth = 0;
    std::size_t reserved_focus_slots = 0;
    int admission_window_ms = 0;
    bool is_adaptive = false;
    std::size_t adaptive_window_size = 0;
    std::size_t adaptive_updates = 0;
    double adaptive_latency_budget_ratio = 0.0;
    int adaptive_latency_budget_ms = 0;
    int adaptive_latency_baseline_p95_ms = 0;
    int focus_queue_p95_target_ms = 0;
    int starvation_threshold_ms = 0;
    int max_admission_window_ms = 0;
    MetricsSummary metrics;
};

class RuntimeService {
public:
    RuntimeService(
        RuntimeServiceConfig service_config = RuntimeServiceConfig{},
        SchedulerConfig scheduler_config = SchedulerConfig{},
        MockBackendConfig backend_config = MockBackendConfig{}
    );
    RuntimeService(
        RuntimeServiceConfig service_config,
        SchedulerConfig scheduler_config,
        std::shared_ptr<Backend> backend
    );
    RuntimeService(
        SchedulerConfig scheduler_config,
        MockBackendConfig backend_config
    );
    ~RuntimeService();

    RuntimeService(const RuntimeService&) = delete;
    RuntimeService& operator=(const RuntimeService&) = delete;

    ScheduleResponse schedule(const ScheduledRequest& req);
    RuntimeServiceSnapshot snapshot() const;

private:
    struct ExecutionThread {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
    };

    struct AdaptiveRecord {
        bool focus = false;
        bool deadline_missed = false;
        int queue_wait_ms = 0;
        int total_latency_ms = 0;
    };

    void dispatcher_loop();
    bool can_dispatch_locked() const;
    void dispatch_ready_turns_locked();
    void launch_backend_execution_locked(RuntimeAdmittedTurn admitted);
    void finish_backend_execution(
        RuntimeAdmittedTurn admitted,
        BackendResult backend_result
    );
    void reap_finished_threads_locked();
    void record_adaptive_feedback_locked(const RuntimeRunResult& result);
    void maybe_update_adaptive_policy_locked();

    RuntimeServiceConfig service_config_;
    SchedulerConfig baseline_scheduler_config_;
    SchedulerConfig scheduler_config_;
    Runtime runtime_;
    MetricsCollector metrics_;
    std::deque<AdaptiveRecord> adaptive_records_;
    std::size_t adaptive_updates_ = 0;
    int adaptive_latency_baseline_p95_ms_ = 0;
    std::thread dispatcher_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::promise<RuntimeRunResult>> pending_;
    std::vector<ExecutionThread> execution_threads_;
    std::size_t inflight_backend_requests_ = 0;
    std::size_t completed_requests_ = 0;
    std::size_t rejected_requests_ = 0;
    bool stopping_ = false;
};

} // namespace ar
