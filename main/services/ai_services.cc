#include "ai_services.h"

#include "board.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <http.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define TAG "ThirdPartyAi"

#ifndef CONFIG_XIAOZHI_THIRD_PARTY_ASR_API_KEY
#define CONFIG_XIAOZHI_THIRD_PARTY_ASR_API_KEY ""
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_TTS_API_KEY
#define CONFIG_XIAOZHI_THIRD_PARTY_TTS_API_KEY ""
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY
#define CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY ""
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_HEADER
#define CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_HEADER "x-bf-vk"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_PREFIX
#define CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_PREFIX ""
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_ASR_MODEL
#define CONFIG_XIAOZHI_THIRD_PARTY_ASR_MODEL "qwen3-asr-flash"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_TTS_MODEL
#define CONFIG_XIAOZHI_THIRD_PARTY_TTS_MODEL "qwen-tts"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_TTS_VOICE
#define CONFIG_XIAOZHI_THIRD_PARTY_TTS_VOICE "Cherry"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_LLM_BASE_URL
#define CONFIG_XIAOZHI_THIRD_PARTY_LLM_BASE_URL "https://vibeg.seer-robotics.cn/openai/v1"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_LLM_MODEL
#define CONFIG_XIAOZHI_THIRD_PARTY_LLM_MODEL "claude-fable-5-1"
#endif
#ifndef CONFIG_XIAOZHI_THIRD_PARTY_DASHSCOPE_BASE_URL
#define CONFIG_XIAOZHI_THIRD_PARTY_DASHSCOPE_BASE_URL "https://maas.qianwenaiapi.com/api/v1"
#endif


namespace {

std::string GetStringOrDefault(cJSON* root, const char* key, const char* default_value = "") {
    auto* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : default_value;
}

std::string ToHex(uint8_t value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out += kHex[(value >> 4) & 0x0F];
    out += kHex[value & 0x0F];
    return out;
}

std::string UrlEncode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += ToHex(c);
        }
    }
    return out;
}


std::optional<int> GetStatusCode(Http& http) {
    auto status = http.GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "HTTP status error: %s", status.error().ToString().c_str());
        return std::nullopt;
    }
    return *status;
}

std::optional<std::string> HttpRequest(NetworkInterface& network,
                                       const std::string& method,
                                       const std::string& url,
                                       const std::vector<std::pair<std::string, std::string>>& headers,
                                       const std::string& body = "") {
    auto http = network.CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "CreateHttp failed");
        return std::nullopt;
    }

    for (const auto& [key, value] : headers) {
        http->SetHeader(key, value);
    }
    if (!body.empty()) {
        http->SetContent(std::string(body));
    }

    auto opened = http->Open(method, url);
    if (!opened) {
        ESP_LOGE(TAG, "HTTP open failed: %s", opened.error().ToString().c_str());
        return std::nullopt;
    }

    auto status = GetStatusCode(*http);
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "HTTP %s %s status=%d body=%s", method.c_str(), url.c_str(),
                 status ? *status : -1, http->ReadAll().c_str());
        return std::nullopt;
    }

    std::string response = http->ReadAll();
    http->Close();
    return response;
}

std::optional<std::string> HttpPostJson(NetworkInterface& network,
                                        const std::string& url,
                                        const std::string& body,
                                        const std::string& authorization = "",
                                        const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
    };
    if (!authorization.empty()) {
        headers.emplace_back("Authorization", authorization);
    }
    for (const auto& [key, value] : extra_headers) {
        headers.emplace_back(key, value);
    }
    return HttpRequest(network, "POST", url, headers, body);
}

std::optional<std::string> HttpGet(NetworkInterface& network,
                                   const std::string& url,
                                   const std::string& authorization = "") {
    std::vector<std::pair<std::string, std::string>> headers;
    if (!authorization.empty()) {
        headers.emplace_back("Authorization", authorization);
    }
    return HttpRequest(network, "GET", url, headers);
}

bool HttpDownload(NetworkInterface& network, const std::string& url, std::vector<uint8_t>& out) {
    auto http = network.CreateHttp(0);
    if (!http) {
        return false;
    }

    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGE(TAG, "Download open failed: %s", opened.error().ToString().c_str());
        return false;
    }

    auto status = GetStatusCode(*http);
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "Download status=%d", status ? *status : -1);
        return false;
    }

    std::string body = http->ReadAll();
    http->Close();
    if (body.empty()) {
        ESP_LOGE(TAG, "Download body empty");
        return false;
    }
    out.assign(body.begin(), body.end());
    return true;
}

uint32_t ReadU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

bool ParseWavPcm(const std::vector<uint8_t>& wav, std::vector<int16_t>& pcm, int& sample_rate) {
    if (wav.size() < 44) {
        return false;
    }

    // Search for the first fmt and data chunks. This accepts standard PCM WAV files
    // produced by DashScope/Oss and simple local test files.
    size_t offset = 12;
    bool have_fmt = false;
    bool have_data = false;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t fmt_sample_rate = 16000;

    while (offset + 8 <= wav.size()) {
        const uint8_t* p = wav.data() + offset;
        uint32_t id = ReadU32(p);
        uint32_t size = ReadU32(p + 4);
        if (id == 0x20746d66) {  // "fmt "
            if (offset + 8 + size <= wav.size() && size >= 16) {
                const uint8_t* fmt = p + 8;
                auto format = ReadU16(fmt + 0);
                channels = ReadU16(fmt + 2);
                fmt_sample_rate = ReadU32(fmt + 4);
                bits_per_sample = ReadU16(fmt + 14);
                have_fmt = (format == 1 && channels == 1 && bits_per_sample == 16);
            }
        } else if (id == 0x61746164) {  // "data"
            data_offset = offset + 8;
            data_size = size;
            have_data = data_offset + data_size <= wav.size();
            break;
        }
        offset += 8 + size + (size & 1);
    }

    if (!have_fmt || !have_data) {
        return false;
    }

    sample_rate = static_cast<int>(fmt_sample_rate);
    size_t sample_count = data_size / 2;
    pcm.resize(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        int16_t sample = static_cast<int16_t>(ReadU16(wav.data() + data_offset + i * 2));
        pcm[i] = sample;
    }
    return true;
}

struct OssUploadPolicy {
    std::string upload_host;
    std::string upload_dir;
    std::string oss_access_key_id;
    std::string signature;
    std::string policy;
    std::string x_oss_object_acl;
    std::string x_oss_forbid_overwrite;
};

std::optional<OssUploadPolicy> FetchOssPolicy(NetworkInterface& network,
                                               const ThirdPartyAiConfig& config,
                                               const std::string& model) {
    std::string url = config.dashscope_base_url + "/uploads?action=getPolicy&model=" + UrlEncode(model);
    std::string auth = "Bearer " + config.asr_api_key;
    auto response = HttpGet(network, url, auth);
    if (!response) {
        return std::nullopt;
    }

    ESP_LOGI(TAG, "OSS policy response: %s", response->c_str());

    cJSON* root = cJSON_Parse(response->c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid OSS policy JSON");
        return std::nullopt;
    }

    // DashScope-compatible gateways may wrap upload-policy fields in `data` or `result`.
    cJSON* policy_json = root;
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        policy_json = data;
    } else {
        cJSON* result = cJSON_GetObjectItem(root, "result");
        if (cJSON_IsObject(result)) {
            policy_json = result;
        }
    }

    OssUploadPolicy policy{
        .upload_host = GetStringOrDefault(policy_json, "upload_host"),
        .upload_dir = GetStringOrDefault(policy_json, "upload_dir"),
        .oss_access_key_id = GetStringOrDefault(policy_json, "oss_access_key_id"),
        .signature = GetStringOrDefault(policy_json, "signature"),
        .policy = GetStringOrDefault(policy_json, "policy"),
        .x_oss_object_acl = GetStringOrDefault(policy_json, "x_oss_object_acl", "private"),
        .x_oss_forbid_overwrite = GetStringOrDefault(policy_json, "x_oss_forbid_overwrite", "false"),
    };
    ESP_LOGI(TAG,
             "OSS policy parsed: host=%s dir=%s acl=%s forbid=%s key_id=%s signature_len=%d policy_len=%d",
             policy.upload_host.c_str(), policy.upload_dir.c_str(), policy.x_oss_object_acl.c_str(),
             policy.x_oss_forbid_overwrite.c_str(), policy.oss_access_key_id.c_str(),
             static_cast<int>(policy.signature.size()), static_cast<int>(policy.policy.size()));
    cJSON_Delete(root);

    if (policy.upload_host.empty() || policy.upload_dir.empty() || policy.policy.empty()) {
        ESP_LOGE(TAG, "OSS upload policy incomplete");
        return std::nullopt;
    }
    return policy;
}

std::optional<std::string> UploadWavToOss(NetworkInterface& network,
                                          const ThirdPartyAiConfig& config,
                                          const std::vector<uint8_t>& wav) {
    const std::string model = config.asr_model;
    auto policy = FetchOssPolicy(network, config, model);
    if (!policy) {
        return std::nullopt;
    }

    std::string file_name = "xiaozhi_qwen_asr.wav";
    std::string key = policy->upload_dir + "/" + file_name;
    std::string boundary = "----XIAOZHI_QWEN_ASR_BOUNDARY";

    std::string body;
    auto add_field = [&](const std::string& name, const std::string& value) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
        body += value + "\r\n";
    };
    auto add_file = [&](const std::string& name, const std::string& file_name,
                        const std::string& content_type, const std::vector<uint8_t>& data) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + file_name + "\"\r\n";
        body += "Content-Type: " + content_type + "\r\n\r\n";
        body.append(reinterpret_cast<const char*>(data.data()), data.size());
        body += "\r\n";
    };

    add_field("OSSAccessKeyId", policy->oss_access_key_id);
    add_field("Signature", policy->signature);
    add_field("policy", policy->policy);
    add_field("key", key);
    add_field("x-oss-object-acl", policy->x_oss_object_acl);
    add_field("x-oss-forbid-overwrite", policy->x_oss_forbid_overwrite);
    add_field("success_action_status", "200");
    add_field("x-oss-content-type", "audio/wav");
    add_file("file", file_name, "audio/wav", wav);
    body += "--" + boundary + "--\r\n";

    auto http = network.CreateHttp(3);
    if (!http) {
        return std::nullopt;
    }

    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    auto opened = http->Open("POST", policy->upload_host);
    if (!opened) {
        ESP_LOGE(TAG, "OSS upload open failed: %s", opened.error().ToString().c_str());
        return std::nullopt;
    }

    size_t written_total = 0;
    while (written_total < body.size()) {
        auto written = http->Write(body.data() + written_total, body.size() - written_total);
        if (!written) {
            ESP_LOGE(TAG, "OSS upload write failed: %s", written.error().ToString().c_str());
            http->Close();
            return std::nullopt;
        }
        if (*written == 0) {
            break;
        }
        written_total += *written;
    }
    // Signal end of upload body. Some HTTP implementations use a zero-length write to flush.
    if (auto flushed = http->Write("", 0); !flushed) {
        ESP_LOGE(TAG, "OSS upload flush failed: %s", flushed.error().ToString().c_str());
        http->Close();
        return std::nullopt;
    }

    auto status = GetStatusCode(*http);
    std::string result = http->ReadAll();
    http->Close();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "OSS upload status=%d body=%s", status ? *status : -1, result.c_str());
        return std::nullopt;
    }
    ESP_LOGI(TAG, "OSS upload result: %s", result.c_str());
    (void)config;
    return "oss://" + key;
}

class QwenAsrService : public SpeechRecognizer {
public:
    QwenAsrService(ThirdPartyAiConfig config, NetworkInterface& network)
        : config_(std::move(config)), network_(network) {}

    std::optional<SpeechRecognitionResult> Recognize(const std::vector<uint8_t>& wav_data) override {
        auto oss_url = UploadWavToOss(network_, config_, wav_data);
        if (!oss_url) {
            return std::nullopt;
        }

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", config_.asr_model.c_str());
        cJSON* input = cJSON_CreateObject();
        cJSON* messages = cJSON_CreateArray();
        cJSON* system_message = cJSON_CreateObject();
        cJSON_AddStringToObject(system_message, "role", "system");
        cJSON_AddStringToObject(system_message, "content", "");
        cJSON_AddItemToArray(messages, system_message);
        cJSON* user_message = cJSON_CreateObject();
        cJSON_AddStringToObject(user_message, "role", "user");
        cJSON* content_array = cJSON_CreateArray();
        cJSON* audio_item = cJSON_CreateObject();
        cJSON_AddStringToObject(audio_item, "audio", oss_url->c_str());
        cJSON_AddItemToArray(content_array, audio_item);
        cJSON_AddItemToObject(user_message, "content", content_array);
        cJSON_AddItemToArray(messages, user_message);
        cJSON_AddItemToObject(input, "messages", messages);
        cJSON_AddItemToObject(root, "input", input);
        cJSON* parameters = cJSON_CreateObject();
        cJSON_AddStringToObject(parameters, "result_format", "message");
        cJSON* asr_options = cJSON_CreateObject();
        cJSON_AddBoolToObject(asr_options, "enable_lid", true);
        cJSON_AddBoolToObject(asr_options, "enable_itn", false);
        cJSON_AddItemToObject(parameters, "asr_options", asr_options);
        cJSON_AddItemToObject(root, "parameters", parameters);

        char* raw = cJSON_PrintUnformatted(root);
        std::string body = raw ? raw : "";
        cJSON_free(raw);
        cJSON_Delete(root);

        std::string endpoint = config_.dashscope_base_url + "/services/aigc/multimodal-generation/generation";
        auto response = HttpPostJson(network_, endpoint, body, "Bearer " + config_.asr_api_key,
                                      {{"X-DashScope-OssResourceResolve", "enable"}});
        if (!response) {
            return std::nullopt;
        }

        cJSON* response_root = cJSON_Parse(response->c_str());
        if (response_root == nullptr) {
            return std::nullopt;
        }

        SpeechRecognitionResult result;
        auto* choices = cJSON_GetObjectItem(response_root, "output");
        if (cJSON_IsObject(choices)) {
            choices = cJSON_GetObjectItem(choices, "choices");
        }
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0 &&
            cJSON_IsObject(cJSON_GetArrayItem(choices, 0))) {
            cJSON* choice = cJSON_GetArrayItem(choices, 0);
            cJSON* message = cJSON_GetObjectItem(choice, "message");
            if (cJSON_IsObject(message)) {
                cJSON* content = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0 &&
                    cJSON_IsObject(cJSON_GetArrayItem(content, 0))) {
                    cJSON* first = cJSON_GetArrayItem(content, 0);
                    auto text = cJSON_GetObjectItem(first, "text");
                    if (cJSON_IsString(text)) {
                        result.text = text->valuestring;
                    }
                }
            }
            cJSON* annotations = cJSON_GetObjectItem(choice, "annotations");
            if (cJSON_IsArray(annotations) && cJSON_GetArraySize(annotations) > 0 &&
                cJSON_IsObject(cJSON_GetArrayItem(annotations, 0))) {
                cJSON* annotation = cJSON_GetArrayItem(annotations, 0);
                result.language = GetStringOrDefault(annotation, "language");
                result.emotion = GetStringOrDefault(annotation, "emotion", "neutral");
            }
        }
        cJSON_Delete(response_root);

        if (result.text.empty()) {
            ESP_LOGE(TAG, "ASR returned no text: %s", response->c_str());
            return std::nullopt;
        }
        return result;
    }

private:
    ThirdPartyAiConfig config_;
    NetworkInterface& network_;
};

class OpenAiCompatibleChatService : public ChatModel {
public:
    OpenAiCompatibleChatService(ThirdPartyAiConfig config, NetworkInterface& network)
        : config_(std::move(config)), network_(network) {}

    std::optional<std::string> Chat(const std::vector<Message>& messages) override {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", config_.llm_model.c_str());
        cJSON* cjson_messages = cJSON_CreateArray();
        for (const auto& message : messages) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "role", message.role.c_str());
            cJSON_AddStringToObject(item, "content", message.content.c_str());
            cJSON_AddItemToArray(cjson_messages, item);
        }
        cJSON_AddItemToObject(root, "messages", cjson_messages);
        char* raw = cJSON_PrintUnformatted(root);
        std::string body = raw ? raw : "";
        cJSON_free(raw);
        cJSON_Delete(root);

        std::string endpoint = config_.llm_base_url;
        if (!endpoint.empty() && endpoint.back() != '/') {
            endpoint += "/";
        }
        endpoint += "chat/completions";

        std::optional<std::string> response;
        if (config_.llm_api_key_header.empty()) {
            response = HttpPostJson(network_, endpoint, body, "Bearer " + config_.llm_api_key);
        } else {
            response = HttpPostJson(network_, endpoint, body, "",
                                    {{config_.llm_api_key_header,
                                      config_.llm_api_key_prefix + config_.llm_api_key}});
        }
        if (!response) {
            return std::nullopt;
        }

        cJSON* response_root = cJSON_Parse(response->c_str());
        if (response_root == nullptr) {
            return std::nullopt;
        }
        std::optional<std::string> text;
        cJSON* choices = cJSON_GetObjectItem(response_root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0 &&
            cJSON_IsObject(cJSON_GetArrayItem(choices, 0))) {
            cJSON* choice = cJSON_GetArrayItem(choices, 0);
            cJSON* message = cJSON_GetObjectItem(choice, "message");
            if (cJSON_IsObject(message)) {
                auto content = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsString(content)) {
                    text = content->valuestring;
                }
            }
        }
        cJSON_Delete(response_root);
        if (!text || text->empty()) {
            ESP_LOGE(TAG, "Chat model returned no text: %s", response->c_str());
        }
        return text;
    }

private:
    ThirdPartyAiConfig config_;
    NetworkInterface& network_;
};

class QwenTtsService : public SpeechSynthesizer {
public:
    QwenTtsService(ThirdPartyAiConfig config, NetworkInterface& network)
        : config_(std::move(config)), network_(network) {}

    std::optional<SpeechSynthesisResult> Synthesize(const std::string& text) override {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", config_.tts_model.c_str());
        cJSON* input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "text", text.c_str());
        cJSON_AddStringToObject(input, "voice", config_.tts_voice.c_str());
        cJSON_AddItemToObject(root, "input", input);
        cJSON* parameters = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "parameters", parameters);

        char* raw = cJSON_PrintUnformatted(root);
        std::string body = raw ? raw : "";
        cJSON_free(raw);
        cJSON_Delete(root);

        std::string endpoint = config_.dashscope_base_url + "/services/aigc/multimodal-generation/generation";
        auto response = HttpPostJson(network_, endpoint, body, "Bearer " + config_.tts_api_key);
        if (!response) {
            return std::nullopt;
        }

        cJSON* response_root = cJSON_Parse(response->c_str());
        if (response_root == nullptr) {
            return std::nullopt;
        }
        std::string audio_url;
        cJSON* output = cJSON_GetObjectItem(response_root, "output");
        if (cJSON_IsObject(output)) {
            cJSON* audio = cJSON_GetObjectItem(output, "audio");
            if (cJSON_IsObject(audio)) {
                audio_url = GetStringOrDefault(audio, "url");
            }
        }
        cJSON_Delete(response_root);

        if (audio_url.empty()) {
            ESP_LOGE(TAG, "TTS returned no audio url: %s", response->c_str());
            return std::nullopt;
        }

        std::vector<uint8_t> wav;
        if (!HttpDownload(network_, audio_url, wav)) {
            return std::nullopt;
        }

        SpeechSynthesisResult result;
        result.ok = ParseWavPcm(wav, result.pcm, result.sample_rate);
        if (!result.ok) {
            result.error_message = "Failed to parse TTS WAV data";
        }
        return result;
    }

private:
    ThirdPartyAiConfig config_;
    NetworkInterface& network_;
};

}  // namespace

ThirdPartyAiConfig LoadThirdPartyAiConfig() {
    ThirdPartyAiConfig config;
    Settings settings("third_party", false);

    config.asr_api_key = settings.GetString("asr_api_key", CONFIG_XIAOZHI_THIRD_PARTY_ASR_API_KEY);
    config.tts_api_key = settings.GetString("tts_api_key", CONFIG_XIAOZHI_THIRD_PARTY_TTS_API_KEY);
    config.llm_api_key = settings.GetString("llm_api_key", CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY);
    config.llm_api_key_header =
        settings.GetString("llm_api_key_header", CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_HEADER);
    config.llm_api_key_prefix =
        settings.GetString("llm_api_key_prefix", CONFIG_XIAOZHI_THIRD_PARTY_LLM_API_KEY_PREFIX);
    config.asr_model = settings.GetString("asr_model", CONFIG_XIAOZHI_THIRD_PARTY_ASR_MODEL);
    config.tts_model = settings.GetString("tts_model", CONFIG_XIAOZHI_THIRD_PARTY_TTS_MODEL);
    config.tts_voice = settings.GetString("tts_voice", CONFIG_XIAOZHI_THIRD_PARTY_TTS_VOICE);
    config.llm_base_url = settings.GetString("llm_base_url", CONFIG_XIAOZHI_THIRD_PARTY_LLM_BASE_URL);
    config.llm_model = settings.GetString("llm_model", CONFIG_XIAOZHI_THIRD_PARTY_LLM_MODEL);
    config.dashscope_base_url =
        settings.GetString("dashscope_base_url", CONFIG_XIAOZHI_THIRD_PARTY_DASHSCOPE_BASE_URL);
    return config;
}

std::unique_ptr<SpeechRecognizer> CreateQwenAsrService(const ThirdPartyAiConfig& config,
                                                       NetworkInterface& network) {
    return std::make_unique<QwenAsrService>(config, network);
}

std::unique_ptr<ChatModel> CreateOpenAiCompatibleChatService(const ThirdPartyAiConfig& config,
                                                             NetworkInterface& network) {
    return std::make_unique<OpenAiCompatibleChatService>(config, network);
}

std::unique_ptr<SpeechSynthesizer> CreateQwenTtsService(const ThirdPartyAiConfig& config,
                                                        NetworkInterface& network) {
    return std::make_unique<QwenTtsService>(config, network);
}
