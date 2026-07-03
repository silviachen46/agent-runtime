#pragma once

#include "backend.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"
#include "types.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ar {

class RuntimeService {
public:
    RuntimeService(
        SchedulerConfig scheduler_config = SchedulerConfig{},
        MockBackendConfig backend_config = MockBackendConfig{}
    );
    ~RuntimeService();

    RuntimeService(const RuntimeService&) = delete;
    RuntimeService& operator=(const RuntimeService&) = delete;

    ScheduleResponse schedule(const ScheduledRequest& req);

private:
    void worker_loop();

    Runtime runtime_;
    std::thread worker_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::promise<RuntimeRunResult>> pending_;
    bool stopping_ = false;
};

} // namespace ar
