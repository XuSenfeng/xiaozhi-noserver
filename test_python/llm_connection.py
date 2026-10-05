import os
from openai import OpenAI

client = OpenAI(
    base_url="https://vibeg.seer-robotics.cn/openai/v1",
    api_key=os.environ.get("OPENAI_API_KEY"),
    default_headers={
        "x-bf-vk": os.environ.get("OPENAI_API_KEY"),
    },
)

response = client.chat.completions.create(
    model="claude-fable-5-1",
    messages=[{"role": "user", "content": "Hello!"}],
)

print(response.choices[0].message.content)
