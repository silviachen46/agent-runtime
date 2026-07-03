import time
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Optional

import requests


@dataclass
class AgentRequest:
    session_id: str
    workflow_id: str
    step_id: str
    prompt: str
    priority: int
    deadline_ms: int
    ttft_target_ms: int
    resume_target_ms: int
    max_tokens: int
    temperature: float
    is_resume: bool


def now_ms() -> int:
    return int(time.time() * 1000)


class SchedulerClient:
    def __init__(self, base_url: str, timeout_s: float = 60.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    def schedule(self, req: AgentRequest) -> Dict[str, Any]:
        request_id = str(uuid.uuid4())

        payload = {
            "request_id": request_id,
            "session_id": req.session_id,
            "workflow_id": req.workflow_id,
            "step_id": req.step_id,
            "priority": req.priority,
            "deadline_ms": req.deadline_ms,
            "ttft_target_ms": req.ttft_target_ms,
            "resume_target_ms": req.resume_target_ms,
            "prompt": req.prompt,
            "max_tokens": req.max_tokens,
            "temperature": req.temperature,
            "metadata": {
                "is_resume": req.is_resume,
                "agent_type": req.workflow_id,
            },
        }

        client_start_ms = now_ms()

        try:
            resp = requests.post(
                f"{self.base_url}/v1/schedule",
                json=payload,
                timeout=self.timeout_s,
            )

            client_end_ms = now_ms()

            row = {
                "request_id": request_id,
                "session_id": req.session_id,
                "workflow_id": req.workflow_id,
                "step_id": req.step_id,
                "priority": req.priority,
                "is_resume": req.is_resume,
                "deadline_ms": req.deadline_ms,
                "client_latency_ms": client_end_ms - client_start_ms,
                "http_status": resp.status_code,
                "error": "",
            }

            if resp.status_code == 200:
                data = resp.json()
                metrics = data.get("metrics", {})

                row.update({
                    "server_status": data.get("status", ""),
                    "queue_wait_ms": metrics.get("queue_wait_ms", -1),
                    "ttft_ms": metrics.get("ttft_ms", -1),
                    "total_latency_ms": metrics.get("total_latency_ms", -1),
                    "output_tokens": metrics.get("output_tokens", -1),
                    "deadline_missed": metrics.get("deadline_missed", False),
                    "output": data.get("output", ""),
                })
            else:
                row.update({
                    "server_status": "http_error",
                    "queue_wait_ms": -1,
                    "ttft_ms": -1,
                    "total_latency_ms": -1,
                    "output_tokens": -1,
                    "deadline_missed": True,
                    "output": "",
                    "error": resp.text[:300],
                })

            return row

        except Exception as e:
            client_end_ms = now_ms()

            return {
                "request_id": request_id,
                "session_id": req.session_id,
                "workflow_id": req.workflow_id,
                "step_id": req.step_id,
                "priority": req.priority,
                "is_resume": req.is_resume,
                "deadline_ms": req.deadline_ms,
                "client_latency_ms": client_end_ms - client_start_ms,
                "http_status": -1,
                "server_status": "client_error",
                "queue_wait_ms": -1,
                "ttft_ms": -1,
                "total_latency_ms": -1,
                "output_tokens": -1,
                "deadline_missed": True,
                "output": "",
                "error": str(e),
            }