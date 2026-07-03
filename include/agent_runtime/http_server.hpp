#pragma once

#include <functional>
#include <string>

#include "types.hpp"

namespace ar {

class HttpServer {
public:
    using ScheduleHandler = std::function<ScheduleResponse(const ScheduledRequest&)>;

    HttpServer(std::string host, int port, ScheduleHandler handler);

    void start();

private:
    std::string host_;
    int port_;
    ScheduleHandler schedule_handler_;
};

} // namespace agent_runtime