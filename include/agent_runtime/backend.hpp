#pragma once

#include "types.hpp"

#include <vector>

namespace ar {

struct MockBackendConfig {
    int initial_ttft_ms = 120;
    int resume_ttft_ms = 60;
    int background_ttft_ms = 200;

    int per_token_ms = 20;
    int default_output_tokens = 32;

    bool sleep_enabled = true;
};

struct BackendResult {
    std::vector<TokenEvent> events;

    int ttft_ms = 0;
    int total_latency_ms = 0;
    int output_tokens = 0;
};

class MockBackend {
public:
    explicit MockBackend(MockBackendConfig config = MockBackendConfig{});

    BackendResult run(const TurnSpec& spec) const;

private:
    int estimate_ttft_ms(TurnType turn_type) const;
    int estimate_output_tokens(const TurnSpec& spec) const;

    MockBackendConfig config_;
};

} // namespace ar