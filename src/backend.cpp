#include "agent_runtime/backend.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace ar {

MockBackend::MockBackend(MockBackendConfig config)
    : config_(config) {}

BackendResult MockBackend::run(const TurnSpec& spec) const {
    BackendResult result;

    result.ttft_ms = estimate_ttft_ms(spec.turn_type);
    result.output_tokens = estimate_output_tokens(spec);
    result.total_latency_ms =
        result.ttft_ms + result.output_tokens * config_.per_token_ms;

    if (config_.sleep_enabled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(result.ttft_ms));
    }

    for (int i = 0; i < result.output_tokens; ++i) {
        TokenEvent event;
        event.text = "tok" + std::to_string(i);
        event.is_first_token = (i == 0);
        event.done = false;

        result.events.push_back(std::move(event));

        if (config_.sleep_enabled) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.per_token_ms)
            );
        }
    }

    TokenEvent done;
    done.done = true;
    result.events.push_back(std::move(done));

    return result;
}

int MockBackend::estimate_ttft_ms(TurnType turn_type) const {
    if (turn_type == TurnType::ResumeGenerate) {
        return config_.resume_ttft_ms;
    }

    if (turn_type == TurnType::BackgroundGenerate) {
        return config_.background_ttft_ms;
    }

    return config_.initial_ttft_ms;
}

int MockBackend::estimate_output_tokens(const TurnSpec& spec) const {
    return std::max(1, std::min(spec.max_tokens, config_.default_output_tokens));
}

} // namespace ar