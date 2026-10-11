# coding=utf-8
import os
import json
import urllib.request

DASHSCOPE_BASE_URL = os.environ.get(
    "DASHSCOPE_BASE_URL", "https://maas.qianwenaiapi.com/api/v1"
)
DASHSCOPE_API_KEY = "sk-f7870d9ab43a4a0c9c5ff4cf213cbb31"

model = os.environ.get("Qwen_TTS_MODEL", "qwen3-tts-flash")
voice = os.environ.get("Qwen_TTS_VOICE", "Cherry")
text = "今天天气怎么样？我们一起去公园散步吧。"

body = {
    "model": model,
    "input": {
        "text": text,
        "voice": voice,
    },
    "parameters": {},
}

url = DASHSCOPE_BASE_URL.rstrip("/") + "/services/aigc/multimodal-generation/generation"
request = urllib.request.Request(
    url,
    data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
    headers={
        "Content-Type": "application/json",
        "Authorization": "Bearer " + DASHSCOPE_API_KEY,
    },
)

with urllib.request.urlopen(request, timeout=30) as response:
    result = json.load(response)

print(json.dumps(result, ensure_ascii=False, indent=2))
