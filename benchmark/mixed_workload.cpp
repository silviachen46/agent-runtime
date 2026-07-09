#include "agent_runtime/backend.hpp"
#include "agent_runtime/metrics.hpp"
#include "agent_runtime/runtime.hpp"
#include "agent_runtime/scheduler.hpp"
#include "agent_runtime/types.hpp"

#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ar;

struct BenchmarkResult {
    MetricsSummary all;
    MetricsSummary focus;
};

struct WorkloadCase {
    std::string name;
    std::string description;
    TurnType focus_turn_type = TurnType::InitialGenerate;
    MockBackendConfig backend_config;
    std::function<void(Runtime&)> populate;
};

struct CliOptions {
    std::string case_name = "all";
    std::string policy_name = "all";
    bool list_cases = false;
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

std::string long_text(const std::string& prefix, int words) {
    std::string result;
    result.reserve(static_cast<std::size_t>(words) * 8);

    for (int i = 0; i < words; ++i) {
        if (!result.empty()) {
            result += ' ';
        }

        result += prefix + "_" + std::to_string(i);
    }

    return result;
}

SessionSpec make_session(
    const std::string& session_id,
    UserVisibility visibility,
    WorkloadKind workload,
    LatencySensitivity latency,
    int priority,
    int ttft_target_ms,
    int resume_target_ms,
    int deadline_ms
) {
    SessionSpec spec;
    spec.session_id = session_id;
    spec.policy.visibility = visibility;
    spec.policy.workload = workload;
    spec.policy.latency = latency;
    spec.policy.priority = priority;
    spec.slo.ttft_target_ms = ttft_target_ms;
    spec.slo.resume_target_ms = resume_target_ms;
    spec.slo.deadline_ms = deadline_ms;
    return spec;
}

TurnSpec make_turn(
    const std::string& session_id,
    TurnType turn_type,
    const std::string& role,
    const std::string& content,
    int max_tokens
) {
    TurnSpec spec;
    spec.session_id = session_id;
    spec.turn_type = turn_type;
    spec.max_tokens = max_tokens;
    spec.temperature = 0.2;
    spec.stream = true;
    spec.messages.push_back(Message{
        .role = role,
        .content = content
    });
    return spec;
}

void submit(
    Runtime& runtime,
    const SessionSpec& session,
    const TurnSpec& turn
) {
    runtime.create_session(session);
    runtime.submit_turn(turn);
}

MockBackendConfig base_backend_config() {
    MockBackendConfig config;
    config.initial_ttft_ms = 10;
    config.resume_ttft_ms = 5;
    config.background_ttft_ms = 10;
    config.per_token_ms = 1;
    config.prefill_per_prompt_token_ms = 0;
    config.default_output_tokens = 8;
    config.sleep_enabled = true;
    return config;
}

void populate_agent_resume(Runtime& runtime) {
    int background_id = 0;

    for (int agent_id = 0; agent_id < 20; ++agent_id) {
        for (int j = 0; j < 5; ++j) {
            const std::string session_id =
                "batch_before_" + std::to_string(background_id++);

            submit(
                runtime,
                make_session(
                    session_id,
                    UserVisibility::Background,
                    WorkloadKind::Batch,
                    LatencySensitivity::Low,
                    0,
                    3000,
                    3000,
                    3000
                ),
                make_turn(
                    session_id,
                    TurnType::BackgroundGenerate,
                    "user",
                    "Run background batch work.",
                    8
                )
            );
        }

        const std::string agent_session_id =
            "foreground_agent_" + std::to_string(agent_id);

        submit(
            runtime,
            make_session(
                agent_session_id,
                UserVisibility::Foreground,
                WorkloadKind::Agent,
                LatencySensitivity::High,
                10,
                800,
                300,
                500
            ),
            make_turn(
                agent_session_id,
                TurnType::ResumeGenerate,
                "tool",
                "Tool result is ready. Resume generation.",
                8
            )
        );
    }

    for (int i = 0; i < 20; ++i) {
        const std::string session_id =
            "batch_tail_" + std::to_string(background_id++);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Background,
                WorkloadKind::Batch,
                LatencySensitivity::Low,
                0,
                3000,
                3000,
                3000
            ),
            make_turn(
                session_id,
                TurnType::BackgroundGenerate,
                "user",
                "Run background tail batch work.",
                8
            )
        );
    }
}

void populate_chat_burst(Runtime& runtime) {
    for (int i = 0; i < 40; ++i) {
        const std::string session_id = "batch_warmup_" + std::to_string(i);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Background,
                WorkloadKind::Batch,
                LatencySensitivity::Low,
                0,
                3000,
                3000,
                4000
            ),
            make_turn(
                session_id,
                TurnType::BackgroundGenerate,
                "user",
                "Background summarization work.",
                12
            )
        );
    }

    for (int i = 0; i < 40; ++i) {
        const std::string session_id = "chat_" + std::to_string(i);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Foreground,
                WorkloadKind::Chat,
                LatencySensitivity::High,
                8,
                500,
                500,
                900
            ),
            make_turn(
                session_id,
                TurnType::InitialGenerate,
                "user",
                "Answer a short interactive chat question.",
                8
            )
        );
    }
}

void populate_rag_long_prefill(Runtime& runtime) {
    int background_id = 0;

    for (int rag_id = 0; rag_id < 18; ++rag_id) {
        for (int j = 0; j < 3; ++j) {
            const std::string session_id =
                "rag_background_" + std::to_string(background_id++);

            submit(
                runtime,
                make_session(
                    session_id,
                    UserVisibility::Background,
                    WorkloadKind::Batch,
                    LatencySensitivity::Low,
                    0,
                    3000,
                    3000,
                    4000
                ),
                make_turn(
                    session_id,
                    TurnType::BackgroundGenerate,
                    "user",
                    "Offline embedding evaluation batch.",
                    8
                )
            );
        }

        const std::string session_id = "rag_" + std::to_string(rag_id);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Foreground,
                WorkloadKind::Chat,
                LatencySensitivity::High,
                7,
                1200,
                1200,
                1800
            ),
            make_turn(
                session_id,
                TurnType::InitialGenerate,
                "user",
                "Use this retrieved context to answer: " +
                    long_text("retrieved_context", 100),
                16
            )
        );
    }
}

void populate_codegen_decode_pressure(Runtime& runtime) {
    int codegen_id = 0;

    for (int chat_id = 0; chat_id < 30; ++chat_id) {
        for (int j = 0; j < 2; ++j) {
            const std::string session_id =
                "codegen_background_" + std::to_string(codegen_id++);

            submit(
                runtime,
                make_session(
                    session_id,
                    UserVisibility::Background,
                    WorkloadKind::Batch,
                    LatencySensitivity::Low,
                    0,
                    4000,
                    4000,
                    5000
                ),
                make_turn(
                    session_id,
                    TurnType::BackgroundGenerate,
                    "user",
                    "Generate a long code solution for offline evaluation.",
                    48
                )
            );
        }

        const std::string session_id = "short_chat_" + std::to_string(chat_id);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Foreground,
                WorkloadKind::Chat,
                LatencySensitivity::High,
                8,
                700,
                700,
                1200
            ),
            make_turn(
                session_id,
                TurnType::InitialGenerate,
                "user",
                "Give a concise answer while code generation is queued.",
                8
            )
        );
    }

    for (int i = 0; i < 15; ++i) {
        const std::string session_id =
            "codegen_tail_" + std::to_string(codegen_id++);

        submit(
            runtime,
            make_session(
                session_id,
                UserVisibility::Background,
                WorkloadKind::Batch,
                LatencySensitivity::Low,
                0,
                4000,
                4000,
                5000
            ),
            make_turn(
                session_id,
                TurnType::BackgroundGenerate,
                "user",
                "Generate another long code solution.",
                48
            )
        );
    }
}

std::vector<WorkloadCase> make_cases() {
    MockBackendConfig agent_backend = base_backend_config();

    MockBackendConfig chat_backend = base_backend_config();
    chat_backend.per_token_ms = 2;
    chat_backend.default_output_tokens = 12;

    MockBackendConfig rag_backend = base_backend_config();
    rag_backend.prefill_per_prompt_token_ms = 1;
    rag_backend.default_output_tokens = 16;

    MockBackendConfig codegen_backend = base_backend_config();
    codegen_backend.per_token_ms = 1;
    codegen_backend.default_output_tokens = 64;

    return {
        WorkloadCase{
            .name = "agent_resume",
            .description = "foreground agent resume turns mixed with background batch work",
            .focus_turn_type = TurnType::ResumeGenerate,
            .backend_config = agent_backend,
            .populate = populate_agent_resume
        },
        WorkloadCase{
            .name = "chat_burst",
            .description = "interactive chat burst arriving behind queued background work",
            .focus_turn_type = TurnType::InitialGenerate,
            .backend_config = chat_backend,
            .populate = populate_chat_burst
        },
        WorkloadCase{
            .name = "rag_long_prefill",
            .description = "RAG answers with long retrieved context and short outputs",
            .focus_turn_type = TurnType::InitialGenerate,
            .backend_config = rag_backend,
            .populate = populate_rag_long_prefill
        },
        WorkloadCase{
            .name = "codegen_decode_pressure",
            .description = "short interactive turns competing with long code-generation decodes",
            .focus_turn_type = TurnType::InitialGenerate,
            .backend_config = codegen_backend,
            .populate = populate_codegen_decode_pressure
        }
    };
}

std::vector<SchedulerConfig> make_policy_configs() {
    SchedulerConfig fifo;
    fifo.policy_kind = SchedulerPolicyKind::Fifo;

    SchedulerConfig priority;
    priority.policy_kind = SchedulerPolicyKind::Priority;

    SchedulerConfig priority_fair;
    priority_fair.policy_kind = SchedulerPolicyKind::PriorityFair;

    SchedulerConfig priority_tail_aging;
    priority_tail_aging.policy_kind = SchedulerPolicyKind::PriorityTailAging;

    SchedulerConfig slo;
    slo.policy_kind = SchedulerPolicyKind::SloAware;

    SchedulerConfig session_aware;
    session_aware.policy_kind = SchedulerPolicyKind::SessionAwareHybrid;

    return {fifo, priority, priority_fair, priority_tail_aging, slo, session_aware};
}

BenchmarkResult run_case(
    const WorkloadCase& workload_case,
    const SchedulerConfig& scheduler_config
) {
    Runtime runtime{
        scheduler_config,
        workload_case.backend_config
    };

    MetricsCollector metrics;

    workload_case.populate(runtime);

    while (true) {
        std::optional<RuntimeRunResult> result = runtime.run_once();
        if (!result.has_value()) {
            break;
        }

        metrics.record(
            result->turn,
            result->backend_result,
            result->queue_wait_ms
        );
    }

    BenchmarkResult result;
    result.all = metrics.summarize_all();
    result.focus = metrics.summarize_turn_type(workload_case.focus_turn_type);
    return result;
}

void print_summary(
    const WorkloadCase& workload_case,
    const SchedulerConfig& scheduler_config,
    const BenchmarkResult& result
) {
    std::cout << "== case=" << workload_case.name
              << " policy="
              << scheduler_policy_name(scheduler_config.policy_kind)
              << " ==\n";

    std::cout << "description: " << workload_case.description << "\n";
    std::cout << "all.count: " << result.all.count << "\n";
    std::cout << "all.avg_queue_wait_ms: "
              << result.all.avg_queue_wait_ms << "\n";
    std::cout << "all.p95_queue_wait_ms: "
              << result.all.p95_queue_wait_ms << "\n";
    std::cout << "all.p95_total_latency_ms: "
              << result.all.p95_total_latency_ms << "\n";
    std::cout << "all.deadline_miss_rate: "
              << result.all.deadline_miss_rate << "\n";

    std::cout << "focus.count: " << result.focus.count << "\n";
    std::cout << "focus.avg_queue_wait_ms: "
              << result.focus.avg_queue_wait_ms << "\n";
    std::cout << "focus.p50_queue_wait_ms: "
              << result.focus.p50_queue_wait_ms << "\n";
    std::cout << "focus.p95_queue_wait_ms: "
              << result.focus.p95_queue_wait_ms << "\n";
    std::cout << "focus.p99_queue_wait_ms: "
              << result.focus.p99_queue_wait_ms << "\n";
    std::cout << "focus.p95_total_latency_ms: "
              << result.focus.p95_total_latency_ms << "\n";
    std::cout << "focus.deadline_miss_rate: "
              << result.focus.deadline_miss_rate << "\n";
    std::cout << "\n";
}

CliOptions parse_args(int argc, char** argv) {
    CliOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: " << argv[0]
                << " [--case=all|agent_resume|chat_burst|rag_long_prefill|codegen_decode_pressure]"
                << " [--policy=all|fifo|priority|priority_fair|priority_tail_aging|slo|session_aware]"
                << " [--list-cases]\n";
            std::exit(0);
        } else if (arg == "--list-cases") {
            options.list_cases = true;
        } else if (starts_with(arg, "--case=")) {
            options.case_name = value_after_equals(arg);
        } else if (starts_with(arg, "--policy=")) {
            options.policy_name = value_after_equals(arg);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    return options;
}

bool case_selected(const WorkloadCase& workload_case, const CliOptions& options) {
    return options.case_name == "all" || options.case_name == workload_case.name;
}

bool policy_selected(
    const SchedulerConfig& scheduler_config,
    const CliOptions& options
) {
    return options.policy_name == "all" ||
           options.policy_name ==
               scheduler_policy_name(scheduler_config.policy_kind) ||
           (options.policy_name == "slo" &&
            scheduler_config.policy_kind == SchedulerPolicyKind::SloAware) ||
           (options.policy_name == "session_aware" &&
            scheduler_config.policy_kind == SchedulerPolicyKind::SessionAwareHybrid);
}

void list_cases(const std::vector<WorkloadCase>& workload_cases) {
    for (const auto& workload_case : workload_cases) {
        std::cout << workload_case.name << ": "
                  << workload_case.description << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const CliOptions options = parse_args(argc, argv);
        const std::vector<WorkloadCase> workload_cases = make_cases();
        const std::vector<SchedulerConfig> policy_configs =
            make_policy_configs();

        if (options.list_cases) {
            list_cases(workload_cases);
            return 0;
        }

        int runs = 0;

        std::cout << std::fixed << std::setprecision(2);

        for (const auto& workload_case : workload_cases) {
            if (!case_selected(workload_case, options)) {
                continue;
            }

            for (const auto& policy_config : policy_configs) {
                if (!policy_selected(policy_config, options)) {
                    continue;
                }

                const BenchmarkResult result =
                    run_case(workload_case, policy_config);
                print_summary(workload_case, policy_config, result);
                ++runs;
            }
        }

        if (runs == 0) {
            throw std::invalid_argument(
                "no benchmark matched --case=" + options.case_name +
                " --policy=" + options.policy_name
            );
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "benchmark error: " << e.what() << "\n";
        return 1;
    }
}
