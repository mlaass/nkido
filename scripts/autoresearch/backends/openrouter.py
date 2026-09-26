"""OpenRouter backend (GLM / DeepSeek / Qwen). Cost comes from the response's
`usage.cost` (requested via `usage: {include: true}`). Needs OPENROUTER_API_KEY."""
import os

from backends.openai_compat import OpenAICompat

# Pinned from the public catalogue (/api/v1/models, tool-capable, 2026-09-24).
# Dated ids rather than `~…-latest` aliases so reruns hit the same weights.
MATRIX = ["z-ai/glm-5.3", "deepseek/deepseek-v4-pro-0813", "qwen/qwen3.8-27b"]


class OpenRouter(OpenAICompat):
    provider = "openrouter"
    base_url = "https://openrouter.ai/api/v1"

    def __init__(self, model):
        super().__init__(model)
        self.key = os.environ.get("OPENROUTER_API_KEY")
        if not self.key:
            raise SystemExit("OPENROUTER_API_KEY is not set")

    def headers(self):
        return {"Content-Type": "application/json", "Authorization": f"Bearer {self.key}",
                "X-Title": "nkido simd-autoresearch"}

    def extra_body(self):
        # Default routing favours the cheapest upstream (Baidu, DeepInfra at 30-40 tok/s),
        # which blew the 30 min wall clock. Same weights, fastest host.
        return {"usage": {"include": True}, "provider": {"sort": "throughput"}}
