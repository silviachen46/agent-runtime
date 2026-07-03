#include <iostream>

#include "agent_runtime/http_server.hpp"
#include "agent_runtime/runtime_service.hpp"

using namespace ar;

int main() {
    RuntimeService runtime_service;

    HttpServer server(
        "0.0.0.0",
        9000,
        [&runtime_service](const ScheduledRequest& req) {
            return runtime_service.schedule(req);
        }
    );

    server.start();

    return 0;
}
