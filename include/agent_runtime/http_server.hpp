#pragma once

#include <functional>
#include <string>

#include "runtime_service.hpp"
#include "types.hpp"

namespace ar {

class HttpServer {
public:
    using ScheduleHandler = std::function<ScheduleResponse(const ScheduledRequest&)>;
    using StateHandler = std::function<RuntimeServiceSnapshot()>;

    HttpServer(
        std::string host,
        int port,
        ScheduleHandler handler,
        StateHandler state_handler = nullptr
    );

    void start();

private:
    std::string host_;
    int port_;
    ScheduleHandler schedule_handler_;
    StateHandler state_handler_;
};

} // namespace agent_runtime
