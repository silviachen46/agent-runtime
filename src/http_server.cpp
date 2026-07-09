#include "agent_runtime/http_server.hpp"

#include <chrono>
#include <exception>
#include <iostream>

#include "../third_party/httplib.h"
#include "../third_party/json.hpp"

namespace ar {

using json = nlohmann::json;

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

ScheduledRequest parse_schedule_request(const json& j) {
    ScheduledRequest req;

    req.request_id = j.value("request_id", "");
    req.session_id = j.value("session_id", "");
    req.workflow_id = j.value("workflow_id", "");
    req.step_id = j.value("step_id", "");

    req.priority = j.value("priority", 0);
    req.deadline_ms = j.value("deadline_ms", 0);
    req.ttft_target_ms = j.value("ttft_target_ms", 0);
    req.resume_target_ms = j.value("resume_target_ms", 0);

    req.prompt = j.value("prompt", "");
    req.max_tokens = j.value("max_tokens", 256);
    req.temperature = j.value("temperature", 0.2);

    req.arrival_ts_ms = now_ms();

    if (j.contains("metadata") && j["metadata"].is_object()) {
        req.is_resume = j["metadata"].value("is_resume", false);
    } else {
        req.is_resume = j.value("is_resume", false);
    }

    return req;
}

json to_json(const ScheduleResponse& resp) {
    return json{
        {"request_id", resp.request_id},
        {"session_id", resp.session_id},
        {"workflow_id", resp.workflow_id},
        {"step_id", resp.step_id},
        {"status", resp.status},
        {"output", resp.output},
        {"metrics", {
            {"queue_wait_ms", resp.queue_wait_ms},
            {"ttft_ms", resp.ttft_ms},
            {"total_latency_ms", resp.total_latency_ms},
            {"output_tokens", resp.output_tokens},
            {"deadline_missed", resp.deadline_missed}
        }}
    };
}

json to_json(const MetricsSummary& summary) {
    return json{
        {"count", summary.count},
        {"queue_wait_ms", {
            {"avg", summary.avg_queue_wait_ms},
            {"p50", summary.p50_queue_wait_ms},
            {"p95", summary.p95_queue_wait_ms},
            {"p99", summary.p99_queue_wait_ms}
        }},
        {"ttft_ms", {
            {"avg", summary.avg_ttft_ms},
            {"p50", summary.p50_ttft_ms},
            {"p95", summary.p95_ttft_ms},
            {"p99", summary.p99_ttft_ms}
        }},
        {"total_latency_ms", {
            {"avg", summary.avg_total_latency_ms},
            {"p50", summary.p50_total_latency_ms},
            {"p95", summary.p95_total_latency_ms},
            {"p99", summary.p99_total_latency_ms}
        }},
        {"avg_output_tokens", summary.avg_output_tokens},
        {"deadline_missed_count", summary.deadline_missed_count},
        {"deadline_miss_rate", summary.deadline_miss_rate}
    };
}

json to_json(const SchedulerConfig& config) {
    return json{
        {"policy", scheduler_policy_name(config.policy_kind)},
        {"foreground_boost", config.foreground_boost},
        {"high_latency_boost", config.high_latency_boost},
        {"medium_latency_boost", config.medium_latency_boost},
        {"low_latency_penalty", config.low_latency_penalty},
        {"resume_turn_boost", config.resume_turn_boost},
        {"latency_sensitive_boost", config.latency_sensitive_boost},
        {"deadline_urgency_weight", config.deadline_urgency_weight},
        {"aging_boost_per_ms", config.aging_boost_per_ms},
        {"token_cost_penalty", config.token_cost_penalty},
        {"tail_aging_threshold_ms", config.tail_aging_threshold_ms},
        {"tail_aging_boost_per_ms", config.tail_aging_boost_per_ms}
    };
}

json to_json(const RuntimeServiceSnapshot& state) {
    return json{
        {"status", "ok"},
        {"scheduler_policy", state.scheduler_policy},
        {"scheduler_config", to_json(state.scheduler_config)},
        {"queue", {
            {"queued_turns", state.queued_turns},
            {"max_runtime_queue_depth", state.max_runtime_queue_depth}
        }},
        {"backend", {
            {"inflight_requests", state.inflight_backend_requests},
            {"max_inflight_requests", state.max_inflight_backend_requests},
            {"reserved_focus_slots", state.reserved_focus_slots},
            {"admission_window_ms", state.admission_window_ms}
        }},
        {"requests", {
            {"completed", state.completed_requests},
            {"rejected", state.rejected_requests}
        }},
        {"adaptive", {
            {"is_adaptive", state.is_adaptive},
            {"adaptive_window_size", state.adaptive_window_size},
            {"adaptive_updates", state.adaptive_updates},
            {"latency_budget_ratio", state.adaptive_latency_budget_ratio},
            {"latency_budget_ms", state.adaptive_latency_budget_ms},
            {"latency_baseline_p95_ms", state.adaptive_latency_baseline_p95_ms},
            {"focus_queue_p95_target_ms", state.focus_queue_p95_target_ms},
            {"starvation_threshold_ms", state.starvation_threshold_ms},
            {"max_admission_window_ms", state.max_admission_window_ms}
        }},
        {"metrics", to_json(state.metrics)}
    };
}

json error_json(const std::string& message) {
    return json{
        {"status", "error"},
        {"error", message}
    };
}

} // namespace

HttpServer::HttpServer(
    std::string host,
    int port,
    ScheduleHandler handler,
    StateHandler state_handler
)
    : host_(std::move(host)),
      port_(port),
      schedule_handler_(std::move(handler)),
      state_handler_(std::move(state_handler)) {}

void HttpServer::start() {
    httplib::Server server;

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json body{
            {"status", "ok"},
            {"service", "agent-runtime-scheduler"}
        };

        res.set_content(body.dump(2), "application/json");
    });

    server.Get("/v1/runtime/state", [this](const httplib::Request&, httplib::Response& res) {
        if (!state_handler_) {
            res.status = 503;
            res.set_content(error_json("runtime state unavailable").dump(2), "application/json");
            return;
        }

        RuntimeServiceSnapshot state = state_handler_();
        res.status = 200;
        res.set_content(to_json(state).dump(2), "application/json");
    });

    server.Post("/v1/schedule", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);

            ScheduledRequest schedule_req = parse_schedule_request(body);

            if (schedule_req.request_id.empty()) {
                res.status = 400;
                res.set_content(error_json("missing request_id").dump(2), "application/json");
                return;
            }

            if (schedule_req.session_id.empty()) {
                res.status = 400;
                res.set_content(error_json("missing session_id").dump(2), "application/json");
                return;
            }

            if (schedule_req.prompt.empty()) {
                res.status = 400;
                res.set_content(error_json("missing prompt").dump(2), "application/json");
                return;
            }

            ScheduleResponse schedule_resp = schedule_handler_(schedule_req);

            res.status = 200;
            res.set_content(to_json(schedule_resp).dump(2), "application/json");
        } catch (const json::parse_error& e) {
            res.status = 400;
            res.set_content(error_json(std::string("invalid json: ") + e.what()).dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(error_json(std::string("internal error: ") + e.what()).dump(2), "application/json");
        }
    });

    std::cout << "[http] listening on " << host_ << ":" << port_ << std::endl;

    server.listen(host_, port_);
}

} // namespace agent_runtime
