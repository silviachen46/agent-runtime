#pragma once

#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ar {

struct BackendResult {
    std::vector<TokenEvent> events;

    int ttft_ms = 0;
    int total_latency_ms = 0;
    int output_tokens = 0;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual BackendResult run(const TurnSpec& spec) const = 0;
};

struct MockBackendConfig {
    int initial_ttft_ms = 120;
    int resume_ttft_ms = 60;
    int background_ttft_ms = 200;

    int per_token_ms = 20;
    int prefill_per_prompt_token_ms = 0;
    int default_output_tokens = 32;

    bool sleep_enabled = true;
};

struct OpenAIBackendConfig {
    std::string base_url = "http://127.0.0.1:8000";
    std::string model;
    std::string api_key;
    int timeout_ms = 60000;
};

class MockBackend : public Backend {
public:
    explicit MockBackend(MockBackendConfig config = MockBackendConfig{});

    BackendResult run(const TurnSpec& spec) const override;

private:
    int estimate_ttft_ms(TurnType turn_type) const;
    int estimate_prompt_tokens(const TurnSpec& spec) const;
    int estimate_output_tokens(const TurnSpec& spec) const;

    MockBackendConfig config_;
};

class OpenAIBackend : public Backend {
public:
    explicit OpenAIBackend(OpenAIBackendConfig config);

    BackendResult run(const TurnSpec& spec) const override;

private:
    OpenAIBackendConfig config_;
};

std::shared_ptr<Backend> make_mock_backend(
    MockBackendConfig config = MockBackendConfig{}
);

std::shared_ptr<Backend> make_openai_backend(OpenAIBackendConfig config);

} // namespace ar
