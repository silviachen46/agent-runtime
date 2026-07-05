#include "agent_runtime/backend.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>

#include "httplib.h"
#include "json.hpp"

namespace ar {

using json = nlohmann::json;

namespace {

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

int estimate_tokens_from_text(const std::string& text) {
    int tokens = 0;
    bool in_token = false;

    for (const char ch : text) {
        const bool is_space = std::isspace(static_cast<unsigned char>(ch));
        if (!is_space && !in_token) {
            ++tokens;
            in_token = true;
        } else if (is_space) {
            in_token = false;
        }
    }

    return std::max(tokens, 1);
}

json messages_to_json(const std::vector<Message>& messages) {
    json result = json::array();

    for (const auto& message : messages) {
        result.push_back(json{
            {"role", message.role},
            {"content", message.content}
        });
    }

    return result;
}

std::string extract_output_text(const json& body) {
    if (!body.contains("choices") || !body["choices"].is_array() ||
        body["choices"].empty()) {
        throw std::runtime_error("OpenAI backend response has no choices");
    }

    const json& choice = body["choices"][0];

    if (choice.contains("message") && choice["message"].is_object()) {
        return choice["message"].value("content", "");
    }

    return choice.value("text", "");
}

int extract_output_tokens(const json& body, const std::string& output) {
    if (body.contains("usage") && body["usage"].is_object()) {
        const json& usage = body["usage"];
        if (usage.contains("completion_tokens")) {
            return usage.value("completion_tokens", 0);
        }
    }

    return estimate_tokens_from_text(output);
}

} // namespace

OpenAIBackend::OpenAIBackend(OpenAIBackendConfig config)
    : config_(std::move(config)) {
    if (config_.base_url.empty()) {
        throw std::invalid_argument("OpenAI backend base_url must not be empty");
    }

    if (config_.model.empty()) {
        throw std::invalid_argument("OpenAI backend model must not be empty");
    }
}

BackendResult OpenAIBackend::run(const TurnSpec& spec) const {
    httplib::Client client(config_.base_url);
    client.set_read_timeout(std::chrono::milliseconds(config_.timeout_ms));
    client.set_write_timeout(std::chrono::milliseconds(config_.timeout_ms));
    client.set_connection_timeout(std::chrono::milliseconds(config_.timeout_ms));

    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");

    if (!config_.api_key.empty()) {
        headers.emplace("Authorization", "Bearer " + config_.api_key);
    }

    const json request = {
        {"model", config_.model},
        {"messages", messages_to_json(spec.messages)},
        {"max_tokens", spec.max_tokens},
        {"temperature", spec.temperature},
        {"stream", false}
    };

    const auto started_at = std::chrono::steady_clock::now();
    const auto response = client.Post(
        "/v1/chat/completions",
        headers,
        request.dump(),
        "application/json"
    );
    const int total_latency_ms = elapsed_ms_since(started_at);

    if (!response) {
        throw std::runtime_error("OpenAI backend request failed");
    }

    if (response->status < 200 || response->status >= 300) {
        throw std::runtime_error(
            "OpenAI backend returned HTTP " +
            std::to_string(response->status) +
            ": " + response->body
        );
    }

    const json body = json::parse(response->body);
    const std::string output = extract_output_text(body);

    BackendResult result;
    result.ttft_ms = total_latency_ms;
    result.total_latency_ms = total_latency_ms;
    result.output_tokens = extract_output_tokens(body, output);

    TokenEvent output_event;
    output_event.text = output;
    output_event.is_first_token = true;
    output_event.done = false;
    result.events.push_back(std::move(output_event));

    TokenEvent done;
    done.done = true;
    result.events.push_back(std::move(done));

    return result;
}

std::shared_ptr<Backend> make_openai_backend(OpenAIBackendConfig config) {
    return std::make_shared<OpenAIBackend>(std::move(config));
}

} // namespace ar
