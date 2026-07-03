#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "agent_runtime/http_server.hpp"
#include "agent_runtime/types.hpp"

using namespace ar;

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

ScheduleResponse handle_schedule_mock(const ScheduledRequest& req) {
    long long start_ms = now_ms();

    // mock queue wait
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    long long first_token_ms = now_ms();

    // mock generation latency
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    long long end_ms = now_ms();

    ScheduleResponse resp;
    resp.request_id = req.request_id;
    resp.session_id = req.session_id;
    resp.workflow_id = req.workflow_id;
    resp.step_id = req.step_id;

    resp.status = "completed";
    resp.output = "[mock output] workflow=" + req.workflow_id
                + ", step=" + req.step_id
                + ", prompt_len=" + std::to_string(req.prompt.size());

    resp.queue_wait_ms = 20;
    resp.ttft_ms = static_cast<int>(first_token_ms - start_ms);
    resp.total_latency_ms = static_cast<int>(end_ms - start_ms);
    resp.output_tokens = 32;

    if (req.deadline_ms > 0) {
        resp.deadline_missed = resp.total_latency_ms > req.deadline_ms;
    }

    return resp;
}

} // namespace

int main() {
    HttpServer server(
        "0.0.0.0",
        9000,
        handle_schedule_mock
    );

    server.start();

    return 0;
}