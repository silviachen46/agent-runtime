#include "agent_runtime/backend.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <thread>

namespace ar {

MockBackend::MockBackend(MockBackendConfig config)
    : config_(config) {}

BackendResult MockBackend::run(const TurnSpec& spec) const {
    BackendResult result;

    const int prefill_ms =
        estimate_prompt_tokens(spec) * config_.prefill_per_prompt_token_ms;

    result.ttft_ms = estimate_ttft_ms(spec.turn_type) + prefill_ms;
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

int MockBackend::estimate_prompt_tokens(const TurnSpec& spec) const {
    int tokens = 0;
    bool in_token = false;

    for (const auto& message : spec.messages) {
        for (const char ch : message.content) {
            const bool is_space = std::isspace(static_cast<unsigned char>(ch));
            if (!is_space && !in_token) {
                ++tokens;
                in_token = true;
            } else if (is_space) {
                in_token = false;
            }
        }

        in_token = false;
    }

    return std::max(tokens, 1);
}

int MockBackend::estimate_output_tokens(const TurnSpec& spec) const {
    return std::max(1, std::min(spec.max_tokens, config_.default_output_tokens));
}

std::shared_ptr<Backend> make_mock_backend(MockBackendConfig config) {
    return std::make_shared<MockBackend>(config);
}

} // namespace ar
