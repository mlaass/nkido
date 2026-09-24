"""Local OpenAI-compatible backend: Ollama (default, localhost:11434) or
LocalAI (AUTORESEARCH_LOCAL_URL=http://localhost:8069/v1). USD = 0; gpu_s is
the summed request time (the GPU is busy for the whole request)."""
import os

from backends.openai_compat import OpenAICompat


class LocalOpenAI(OpenAICompat):
    provider = "local"
    local = True

    def __init__(self, model):
        super().__init__(model)
        self.base_url = os.environ.get("AUTORESEARCH_LOCAL_URL", "http://localhost:11434/v1")
