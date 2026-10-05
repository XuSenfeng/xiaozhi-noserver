import os
import sys
import dashscope
dashscope.base_http_api_url = "https://maas.qianwenaiapi.com/api/v1"

messages = [
    {
        "role": "system",
        "content": [
            # 此处用于配置定制化识别的Context
            {"text": ""},
        ]
    },
    {
        "role": "user",
        "content": [
            {"audio": sys.argv[1] if len(sys.argv) > 1 else ""},
        ]
    }
]
response = dashscope.MultiModalConversation.call(
    # 若没有配置环境变量，请用百炼API Key将下行替换为：api_key = "sk-xxx"
    api_key=os.environ.get("DASHSCOPE_API_KEY"),
    model="qwen3-asr-flash",
    messages=messages,
    result_format="message",
    asr_options={
        # "language": "zh", # 可选，若已知音频的语种，可通过该参数指定待识别语种，以提升识别准确率
        "enable_lid":True,
        "enable_itn":False
    }
)
print(response)

"""
{"status_code": 200, "request_id": "a7af47d6-65bb-9f56-b654-35d6d86269eb", "code": "", "message": "", "output": {"text": null, "finish_reason": null, "choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": [{"text": "那我来给大家推荐一款T恤，这款呢真的是超级好看，这个颜色呢很显气质，而且呢也是搭配的绝佳单品，大家可以闭眼入，真的是非常好看，对身材的包容性也很好，不管啥身材的宝宝呢穿上去都是很好看的，推荐宝宝们下单哦。"}], "annotations": [{"emotion": "neutral", "language": "zh", "type": "audio_info"}]}}], "audio": null}, "usage": {"input_tokens": 499, "output_tokens": 73, "characters": 0, "audio_tokens": 499, "input_tokens_details": {"audio_tokens": 499, "text_tokens": 0}, "output_tokens_details": {"text_tokens": 73}, "seconds": 19, "total_tokens": 572}}
"""