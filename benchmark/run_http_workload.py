#!/usr/bin/env python3
import argparse
import concurrent.futures
import json
import statistics
import time
import uuid
from pathlib import Path
from typing import Any, Dict, Iterable, List

import requests

from agent_api import AgentRequest, SchedulerClient


CASES = {
    "agent_resume",
    "chat_burst",
    "rag_long_prefill",
    "codegen_decode_pressure",
}

MODES = {
    "runtime",
    "direct-openai",
}


def long_text(prefix: str, words: int) -> str:
    return " ".join(f"{prefix}_{i}" for i in range(words))


def make_agent_resume() -> List[AgentRequest]:
    requests: List[AgentRequest] = []
    background_id = 0

    for agent_id in range(20):
        for _ in range(5):
            session_id = f"batch_before_{background_id}"
            requests.append(
                AgentRequest(
                    session_id=session_id,
                    workflow_id="agent_resume",
                    step_id="background",
                    prompt="Run background batch work.",
                    priority=0,
                    deadline_ms=3000,
                    ttft_target_ms=3000,
                    resume_target_ms=3000,
                    max_tokens=8,
                    temperature=0.2,
                    is_resume=False,
                )
            )
            background_id += 1

        requests.append(
            AgentRequest(
                session_id=f"foreground_agent_{agent_id}",
                workflow_id="agent_resume",
                step_id="resume",
                prompt="Tool result is ready. Resume generation.",
                priority=10,
                deadline_ms=5000,
                ttft_target_ms=800,
                resume_target_ms=300,
                max_tokens=64,
                temperature=0.2,
                is_resume=True,
            )
        )

    for _ in range(20):
        session_id = f"batch_tail_{background_id}"
        requests.append(
            AgentRequest(
                session_id=session_id,
                workflow_id="agent_resume",
                step_id="background_tail",
                prompt="Run background tail batch work.",
                priority=0,
                deadline_ms=3000,
                ttft_target_ms=3000,
                resume_target_ms=3000,
                max_tokens=8,
                temperature=0.2,
                is_resume=False,
            )
        )
        background_id += 1

    return requests


def make_chat_burst() -> List[AgentRequest]:
    requests: List[AgentRequest] = []

    for i in range(40):
        requests.append(
            AgentRequest(
                session_id=f"batch_warmup_{i}",
                workflow_id="chat_burst",
                step_id="background",
                prompt="Background summarization work.",
                priority=0,
                deadline_ms=4000,
                ttft_target_ms=3000,
                resume_target_ms=3000,
                max_tokens=32,
                temperature=0.2,
                is_resume=False,
            )
        )

    for i in range(40):
        requests.append(
            AgentRequest(
                session_id=f"chat_{i}",
                workflow_id="chat_burst",
                step_id="interactive_chat",
                prompt="Answer a short interactive chat question.",
                priority=8,
                deadline_ms=2000,
                ttft_target_ms=500,
                resume_target_ms=500,
                max_tokens=64,
                temperature=0.2,
                is_resume=False,
            )
        )

    return requests


def make_rag_long_prefill() -> List[AgentRequest]:
    requests: List[AgentRequest] = []
    background_id = 0

    for rag_id in range(18):
        for _ in range(3):
            requests.append(
                AgentRequest(
                    session_id=f"rag_background_{background_id}",
                    workflow_id="rag_long_prefill",
                    step_id="background",
                    prompt="Offline embedding evaluation batch.",
                    priority=0,
                    deadline_ms=4000,
                    ttft_target_ms=3000,
                    resume_target_ms=3000,
                    max_tokens=8,
                    temperature=0.2,
                    is_resume=False,
                )
            )
            background_id += 1

        requests.append(
            AgentRequest(
                session_id=f"rag_{rag_id}",
                workflow_id="rag_long_prefill",
                step_id="rag_answer",
                prompt=(
                    "Use this retrieved context to answer: "
                    + long_text("retrieved_context", 700)
                ),
                priority=7,
                deadline_ms=8000,
                ttft_target_ms=1200,
                resume_target_ms=1200,
                max_tokens=128,
                temperature=0.2,
                is_resume=False,
            )
        )

    return requests


def make_codegen_decode_pressure() -> List[AgentRequest]:
    requests: List[AgentRequest] = []
    codegen_id = 0

    for chat_id in range(30):
        for _ in range(2):
            requests.append(
                AgentRequest(
                    session_id=f"codegen_background_{codegen_id}",
                    workflow_id="codegen_decode_pressure",
                    step_id="long_codegen",
                    prompt="Generate a long code solution for offline evaluation.",
                    priority=0,
                    deadline_ms=10000,
                    ttft_target_ms=4000,
                    resume_target_ms=4000,
                    max_tokens=256,
                    temperature=0.2,
                    is_resume=False,
                )
            )
            codegen_id += 1

        requests.append(
            AgentRequest(
                session_id=f"short_chat_{chat_id}",
                workflow_id="codegen_decode_pressure",
                step_id="short_interactive",
                prompt="Give a concise answer while code generation is queued.",
                priority=8,
                deadline_ms=3000,
                ttft_target_ms=700,
                resume_target_ms=700,
                max_tokens=64,
                temperature=0.2,
                is_resume=False,
            )
        )

    for _ in range(15):
        requests.append(
            AgentRequest(
                session_id=f"codegen_tail_{codegen_id}",
                workflow_id="codegen_decode_pressure",
                step_id="long_codegen_tail",
                prompt="Generate another long code solution.",
                priority=0,
                deadline_ms=10000,
                ttft_target_ms=4000,
                resume_target_ms=4000,
                max_tokens=256,
                temperature=0.2,
                is_resume=False,
            )
        )
        codegen_id += 1

    return requests


def make_requests(case_name: str) -> List[AgentRequest]:
    if case_name == "agent_resume":
        return make_agent_resume()
    if case_name == "chat_burst":
        return make_chat_burst()
    if case_name == "rag_long_prefill":
        return make_rag_long_prefill()
    if case_name == "codegen_decode_pressure":
        return make_codegen_decode_pressure()
    raise ValueError(f"unknown case: {case_name}")


def now_ms() -> int:
    return int(time.time() * 1000)


def estimate_tokens(text: str) -> int:
    return max(1, len(text.split()))


class DirectOpenAIClient:
    def __init__(
        self,
        base_url: str,
        model: str,
        api_key: str = "",
        timeout_s: float = 300.0,
    ):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.timeout_s = timeout_s

    def schedule(self, req: AgentRequest) -> Dict[str, Any]:
        request_id = str(uuid.uuid4())
        payload = {
            "model": self.model,
            "messages": [
                {
                    "role": "user",
                    "content": req.prompt,
                }
            ],
            "max_tokens": req.max_tokens,
            "temperature": req.temperature,
            "stream": False,
        }

        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"

        client_start_ms = now_ms()

        try:
            resp = requests.post(
                f"{self.base_url}/v1/chat/completions",
                json=payload,
                headers=headers,
                timeout=self.timeout_s,
            )

            client_end_ms = now_ms()
            client_latency_ms = client_end_ms - client_start_ms

            row = {
                "request_id": request_id,
                "session_id": req.session_id,
                "workflow_id": req.workflow_id,
                "step_id": req.step_id,
                "priority": req.priority,
                "is_resume": req.is_resume,
                "deadline_ms": req.deadline_ms,
                "client_latency_ms": client_latency_ms,
                "http_status": resp.status_code,
                "mode": "direct-openai",
                "error": "",
            }

            if resp.status_code == 200:
                data = resp.json()
                output = ""
                choices = data.get("choices", [])
                if choices:
                    choice = choices[0]
                    message = choice.get("message", {})
                    output = message.get("content", choice.get("text", ""))

                usage = data.get("usage", {})
                output_tokens = usage.get(
                    "completion_tokens",
                    estimate_tokens(output),
                )

                row.update({
                    "server_status": "completed",
                    "queue_wait_ms": 0,
                    "ttft_ms": client_latency_ms,
                    "total_latency_ms": client_latency_ms,
                    "output_tokens": output_tokens,
                    "deadline_missed": (
                        req.deadline_ms > 0 and
                        client_latency_ms > req.deadline_ms
                    ),
                    "output": output,
                })
            else:
                row.update({
                    "server_status": "http_error",
                    "queue_wait_ms": 0,
                    "ttft_ms": -1,
                    "total_latency_ms": client_latency_ms,
                    "output_tokens": -1,
                    "deadline_missed": True,
                    "output": "",
                    "error": resp.text[:300],
                })

            return row

        except Exception as e:
            client_end_ms = now_ms()
            client_latency_ms = client_end_ms - client_start_ms

            return {
                "request_id": request_id,
                "session_id": req.session_id,
                "workflow_id": req.workflow_id,
                "step_id": req.step_id,
                "priority": req.priority,
                "is_resume": req.is_resume,
                "deadline_ms": req.deadline_ms,
                "client_latency_ms": client_latency_ms,
                "http_status": -1,
                "mode": "direct-openai",
                "server_status": "client_error",
                "queue_wait_ms": 0,
                "ttft_ms": -1,
                "total_latency_ms": client_latency_ms,
                "output_tokens": -1,
                "deadline_missed": True,
                "output": "",
                "error": str(e),
            }


def percentile(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int((len(ordered) - 1) * q)
    return float(ordered[idx])


def average(values: List[float]) -> float:
    if not values:
        return 0.0
    return float(statistics.fmean(values))


def summarize_rows(
    rows: List[Dict[str, Any]],
    case_name: str,
    mode: str,
) -> Dict[str, Any]:
    ok_rows = [
        row
        for row in rows
        if row.get("http_status") == 200 and row.get("server_status") == "completed"
    ]
    focus_rows = [
        row
        for row in ok_rows
        if row.get("priority", 0) > 0 or row.get("is_resume", False)
    ]

    def block(subset: List[Dict[str, Any]]) -> Dict[str, Any]:
        queue_waits = [row.get("queue_wait_ms", 0) for row in subset]
        ttfts = [row.get("ttft_ms", 0) for row in subset]
        total_latencies = [row.get("total_latency_ms", 0) for row in subset]
        client_latencies = [row.get("client_latency_ms", 0) for row in subset]
        output_tokens = [row.get("output_tokens", 0) for row in subset]
        deadline_misses = sum(1 for row in subset if row.get("deadline_missed"))

        return {
            "count": len(subset),
            "queue_wait_ms": {
                "avg": average(queue_waits),
                "p50": percentile(queue_waits, 0.50),
                "p95": percentile(queue_waits, 0.95),
                "p99": percentile(queue_waits, 0.99),
            },
            "ttft_ms": {
                "avg": average(ttfts),
                "p50": percentile(ttfts, 0.50),
                "p95": percentile(ttfts, 0.95),
                "p99": percentile(ttfts, 0.99),
            },
            "total_latency_ms": {
                "avg": average(total_latencies),
                "p50": percentile(total_latencies, 0.50),
                "p95": percentile(total_latencies, 0.95),
                "p99": percentile(total_latencies, 0.99),
            },
            "client_latency_ms": {
                "avg": average(client_latencies),
                "p50": percentile(client_latencies, 0.50),
                "p95": percentile(client_latencies, 0.95),
                "p99": percentile(client_latencies, 0.99),
            },
            "avg_output_tokens": average(output_tokens),
            "deadline_missed_count": deadline_misses,
            "deadline_miss_rate": (
                deadline_misses / len(subset) if subset else 0.0
            ),
        }

    return {
        "case": case_name,
        "mode": mode,
        "total": len(rows),
        "completed": len(ok_rows),
        "failed": len(rows) - len(ok_rows),
        "all": block(ok_rows),
        "focus": block(focus_rows),
    }


def write_jsonl(path: Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, sort_keys=True) + "\n")


def run_workload(args: argparse.Namespace) -> List[Dict[str, Any]]:
    if args.mode == "runtime":
        client = SchedulerClient(args.base_url, timeout_s=args.timeout_s)
    elif args.mode == "direct-openai":
        client = DirectOpenAIClient(
            args.base_url,
            args.model,
            api_key=args.api_key,
            timeout_s=args.timeout_s,
        )
    else:
        raise ValueError(f"unknown mode: {args.mode}")

    requests = make_requests(args.case)
    rows: List[Dict[str, Any]] = []

    start = time.time()

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=args.concurrency
    ) as executor:
        future_to_index = {
            executor.submit(client.schedule, req): idx
            for idx, req in enumerate(requests)
        }

        for future in concurrent.futures.as_completed(future_to_index):
            idx = future_to_index[future]
            row = future.result()
            row["case"] = args.case
            row["mode"] = args.mode
            row["request_index"] = idx
            rows.append(row)

            if args.progress and len(rows) % args.progress == 0:
                print(f"completed {len(rows)}/{len(requests)}")

    rows.sort(key=lambda row: row["request_index"])

    elapsed_s = time.time() - start
    for row in rows:
        row["benchmark_elapsed_s"] = elapsed_s

    return rows


def print_summary(summary: Dict[str, Any]) -> None:
    print(json.dumps(summary, indent=2, sort_keys=True))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run HTTP workloads through the runtime or directly against an OpenAI-compatible backend."
    )
    parser.add_argument("--mode", default="runtime", choices=sorted(MODES))
    parser.add_argument("--case", required=True, choices=sorted(CASES))
    parser.add_argument("--base-url", default="http://127.0.0.1:9000")
    parser.add_argument("--model", default="")
    parser.add_argument("--api-key", default="")
    parser.add_argument("--concurrency", type=int, default=32)
    parser.add_argument("--timeout-s", type=float, default=300.0)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--summary-output", type=Path, default=None)
    parser.add_argument("--progress", type=int, default=20)
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.concurrency <= 0:
        raise ValueError("--concurrency must be positive")

    if args.mode == "direct-openai" and not args.model:
        raise ValueError("--model is required with --mode=direct-openai")

    rows = run_workload(args)
    summary = summarize_rows(rows, args.case, args.mode)

    if args.output:
        write_jsonl(args.output, rows)

    if args.summary_output:
        args.summary_output.parent.mkdir(parents=True, exist_ok=True)
        args.summary_output.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    print_summary(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
