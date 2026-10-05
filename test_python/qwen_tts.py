# DashScope SDK 版本不低于 1.23.1
import os
import dashscope

text = "那我来给大家推荐一款T恤，这款呢真的是超级好看，这个颜色呢很显气质，而且呢也是搭配的绝佳单品，大家可以闭眼入，真的是非常好看，对身材的包容性也很好，不管啥身材的宝宝呢，穿上去都是很好看的。推荐宝宝们下单哦。"
response = dashscope.audio.qwen_tts.SpeechSynthesizer.call(
    # 仅支持qwen-tts系列模型，请勿使用除此之外的其他模型
    model="qwen-tts",
    # 若没有配置环境变量，请用阿里云百炼API Key将下行替换为：api_key="sk-xxx"
    api_key=os.environ.get("DASHSCOPE_API_KEY"),
    text=text,
    voice="Cherry",
)
print(response)

"""
{"status_code": 200, "request_id": "8a7f9724-443d-97a4-95f2-2b9487b48f2f", "code": "", "message": "", "output": {"finish_reason": "stop", "audio": {"data": "", "expires_at": 1791278582, "id": "audio_8a7f9724-443d-97a4-95f2-2b9487b48f2f", "url": "http://dashscope-result-wlcb.oss-cn-wulanchabu.aliyuncs.com/1d/1b/20261005/e6c1b9cc/50777c62-4589-4416-9a35-d35a2c8852e7.wav?Expires=1791278582&OSSAccessKeyId=LTAI5tKPD3TMqf2Lna1fASuh&Signature=wESOnWuM0hrTWkRUYscjJ9BFkng%3D"}}, "usage": {"input_tokens": 76, "output_tokens": 1011, "characters": 0, "input_tokens_details": {"text_tokens": 76}, "output_tokens_details": {"audio_tokens": 1011, "text_tokens": 0}, "total_tokens": 1087}}{"status_code": 200, "request_id": "8a7f9724-443d-97a4-95f2-2b9487b48f2f", "code": "", "message": "", "output": {"finish_reason": "stop", "audio": {"data": "", "expires_at": 1791278582, "id": "audio_8a7f9724-443d-97a4-95f2-2b9487b48f2f", "url": "http://dashscope-result-wlcb.oss-cn-wulanchabu.aliyuncs.com/1d/1b/20261005/e6c1b9cc/50777c62-4589-4416-9a35-d35a2c8852e7.wav?Expires=1791278582&OSSAccessKeyId=LTAI5tKPD3TMqf2Lna1fASuh&Signature=wESOnWuM0hrTWkRUYscjJ9BFkng%3D"}}, "usage": {"input_tokens": 76, "output_tokens": 1011, "characters**: 0,("text_tokens"): 76},("output_tokens_details"): {("audio_tokens"): 1011, ("text_tokens"): 0}, ("total_tokens"): 1087}}
"""