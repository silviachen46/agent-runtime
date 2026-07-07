#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "agent_runtime/backend.hpp"
#include "agent_runtime/http_server.hpp"
#include "agent_runtime/runtime_service.hpp"

using namespace ar;

namespace {

struct AppConfig {
    std::string backend = "mock";
    OpenAIBackendConfig openai;
    RuntimeServiceConfig service;
    SchedulerConfig scheduler;
};

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string value_after_equals(const std::string& arg) {
    const auto pos = arg.find('=');
    if (pos == std::string::npos) {
        throw std::invalid_argument("expected --key=value argument: " + arg);
    }

    return arg.substr(pos + 1);
}

SchedulerPolicyKind parse_policy_kind(const std::string& value) {
    if (value == "fifo") {
        return SchedulerPolicyKind::Fifo;
    }

    if (value == "priority") {
        return SchedulerPolicyKind::Priority;
    }

    if (value == "slo" || value == "slo_aware") {
        return SchedulerPolicyKind::SloAware;
    }

    if (value == "session_aware" || value == "session_aware_hybrid") {
        return SchedulerPolicyKind::SessionAwareHybrid;
    }

    throw std::invalid_argument("unknown scheduler policy: " + value);
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --backend=mock|openai\n"
        << "  --backend-url=http://host:port\n"
        << "  --model=MODEL\n"
        << "  --api-key=KEY\n"
        << "  --max-inflight=N\n"
        << "  --max-queue=N\n"
        << "  --admission-window-ms=N\n"
        << "  --is-adaptive=true|false\n"
        << "  --adaptive-window-size=N\n"
        << "  --policy=fifo|priority|slo|session_aware\n";
}

AppConfig parse_args(int argc, char** argv) {
    AppConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (starts_with(arg, "--backend=")) {
            config.backend = value_after_equals(arg);
        } else if (starts_with(arg, "--backend-url=")) {
            config.openai.base_url = value_after_equals(arg);
        } else if (starts_with(arg, "--model=")) {
            config.openai.model = value_after_equals(arg);
        } else if (starts_with(arg, "--api-key=")) {
            config.openai.api_key = value_after_equals(arg);
        } else if (starts_with(arg, "--max-inflight=")) {
            config.service.max_inflight_backend_requests =
                static_cast<std::size_t>(
                    std::stoul(value_after_equals(arg))
                );
        } else if (starts_with(arg, "--max-queue=")) {
            config.service.max_runtime_queue_depth =
                static_cast<std::size_t>(
                    std::stoul(value_after_equals(arg))
                );
        } else if (starts_with(arg, "--admission-window-ms=")) {
            config.service.admission_window_ms =
                std::stoi(value_after_equals(arg));
        } else if (starts_with(arg, "--is-adaptive=")) {
            const std::string value = value_after_equals(arg);
            if (value == "true" || value == "1" || value == "yes") {
                config.service.is_adaptive = true;
            } else if (value == "false" || value == "0" || value == "no") {
                config.service.is_adaptive = false;
            } else {
                throw std::invalid_argument(
                    "--is-adaptive must be true or false"
                );
            }
        } else if (starts_with(arg, "--adaptive-window-size=")) {
            config.service.adaptive_window_size =
                static_cast<std::size_t>(
                    std::stoul(value_after_equals(arg))
                );
        } else if (starts_with(arg, "--policy=")) {
            config.scheduler.policy_kind =
                parse_policy_kind(value_after_equals(arg));
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (config.backend != "mock" && config.backend != "openai") {
        throw std::invalid_argument("backend must be mock or openai");
    }

    if (config.backend == "openai" && config.openai.model.empty()) {
        throw std::invalid_argument("--model is required when --backend=openai");
    }

    return config;
}

std::shared_ptr<Backend> make_backend_from_config(const AppConfig& config) {
    if (config.backend == "openai") {
        return make_openai_backend(config.openai);
    }

    return make_mock_backend();
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;

    try {
        config = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    RuntimeService runtime_service(
        config.service,
        config.scheduler,
        make_backend_from_config(config)
    );

    HttpServer server(
        "0.0.0.0",
        9000,
        [&runtime_service](const ScheduledRequest& req) {
            return runtime_service.schedule(req);
        },
        [&runtime_service] {
            return runtime_service.snapshot();
        }
    );

    server.start();

    return 0;
}
