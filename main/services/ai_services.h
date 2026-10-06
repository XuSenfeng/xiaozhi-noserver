#ifndef AI_SERVICES_H
#define AI_SERVICES_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "psram_vector.h"

class Http;
class NetworkInterface;

struct ThirdPartyAiConfig {
    // Kconfig compiled defaults; runtime settings may override these values.
    std::string asr_api_key;
    std::string tts_api_key;
    std::string llm_api_key;
    std::string llm_api_key_header = "x-bf-vk";
    std::string llm_api_key_prefix;
    std::string asr_model = "qwen3-asr-flash";
    std::string tts_model = "qwen-tts";
    std::string tts_voice = "Cherry";
    std::string llm_base_url = "https://vibeg.seer-robotics.cn/openai/v1";
    std::string llm_model = "claude-fable-5-1";
    std::string dashscope_base_url = "https://maas.qianwenaiapi.com/api/v1";
};

struct SpeechRecognitionResult {
    std::string text;
    std::string language;
    std::string emotion;
};

class SpeechRecognizer {
public:
    virtual ~SpeechRecognizer() = default;
    virtual std::optional<SpeechRecognitionResult> Recognize(
        const std::vector<uint8_t>& wav_data) = 0;
};

struct ToolDefinition {
    std::string name;      // OpenAI function name, sanitized to supported characters.
    std::string mcp_name;  // Original local MCP tool name; empty means same as name.
    std::string description;
    // OpenAI tools[].function.parameters object as JSON.
    std::string parameters_json;
};

struct ToolCall {
    std::string id;
    std::string name;
    // JSON object string passed in OpenAI `arguments`.
    std::string arguments;
};

struct ChatResult {
    std::optional<std::string> content;
    std::vector<ToolCall> tool_calls;
};

class ChatModel {
public:
    struct Message {
        std::string role;
        std::string content;
        // Assistant messages may include tool calls instead of/in addition to content.
        std::vector<ToolCall> tool_calls;
        // Tool-result messages use role=tool and include tool_call_id/tool_name.
        std::string tool_call_id;
        std::string tool_name;
    };

    virtual ~ChatModel() = default;
    virtual std::optional<ChatResult> Chat(const std::vector<Message>& messages,
                                           const std::vector<ToolDefinition>& tools) = 0;
};

struct SpeechSynthesisResult {
    bool ok = false;
    std::string error_message;
    // PCM samples, mono 16-bit data as returned by provider after conversion.
    PsramVector<int16_t> pcm;
    int sample_rate = 0;
};

class SpeechSynthesizer {
public:
    virtual ~SpeechSynthesizer() = default;
    virtual std::optional<SpeechSynthesisResult> Synthesize(const std::string& text) = 0;
};

std::unique_ptr<SpeechRecognizer> CreateQwenAsrService(const ThirdPartyAiConfig& config,
                                                       NetworkInterface& network);
std::unique_ptr<ChatModel> CreateOpenAiCompatibleChatService(const ThirdPartyAiConfig& config,
                                                             NetworkInterface& network);
std::unique_ptr<SpeechSynthesizer> CreateQwenTtsService(const ThirdPartyAiConfig& config,
                                                        NetworkInterface& network);

ThirdPartyAiConfig LoadThirdPartyAiConfig();

#endif  // AI_SERVICES_H
