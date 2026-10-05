#include "third_party_protocol.h"

#include "application.h"
#include "board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_audio_types.h>
#include <esp_opus_dec.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#define TAG "ThirdPartyProtocol"

namespace {

esp_opus_dec_frame_duration_t FrameDurationEnum(int frame_duration_ms) {
    switch (frame_duration_ms) {
        case 5:
            return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
        case 10:
            return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        case 20:
            return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        case 40:
            return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        case 60:
            return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        case 80:
            return ESP_OPUS_DEC_FRAME_DURATION_80_MS;
        case 100:
            return ESP_OPUS_DEC_FRAME_DURATION_100_MS;
        case 120:
            return ESP_OPUS_DEC_FRAME_DURATION_120_MS;
        default:
            return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    }
}

void WriteU16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void WriteU32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

}  // namespace

ThirdPartyProtocol::ThirdPartyProtocol()
    : config_(LoadThirdPartyAiConfig()), network_(*Board::GetInstance().GetNetwork()) {
    asr_ = CreateQwenAsrService(config_, network_);
    llm_ = CreateOpenAiCompatibleChatService(config_, network_);
    tts_ = CreateQwenTtsService(config_, network_);

    server_sample_rate_ = 16000;
    server_frame_duration_ = 60;
}

ThirdPartyProtocol::~ThirdPartyProtocol() {
    StopWorker();
}

bool ThirdPartyProtocol::Start() {
    if (started_.exchange(true)) {
        return true;
    }
    if (asr_ == nullptr || llm_ == nullptr || tts_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create third-party AI services");
        return false;
    }
    if (on_connected_) {
        on_connected_();
    }
    return true;
}

bool ThirdPartyProtocol::OpenAudioChannel() {
    const bool was_opened = channel_opened_.exchange(true);
    abort_requested_ = false;
    stop_requested_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorded_packets_.clear();
    }

    if (!was_opened && on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void ThirdPartyProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    const bool was_opened = channel_opened_.exchange(false);
    StopWorker();
    abort_requested_ = false;
    stop_requested_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorded_packets_.clear();
    }

    if (was_opened && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool ThirdPartyProtocol::IsAudioChannelOpened() const { return channel_opened_.load(); }

bool ThirdPartyProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!channel_opened_.load() || abort_requested_.load()) {
        return false;
    }
    if (packet == nullptr || packet->payload.empty()) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorded_packets_.push_back(std::move(packet));
    }
    return true;
}

void ThirdPartyProtocol::SendStartListening(ListeningMode mode) {
    if (!channel_opened_.load()) {
        return;
    }
    listening_mode_ = mode;
    abort_requested_ = false;
    stop_requested_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorded_packets_.clear();
    }

    StartWorker();
}

void ThirdPartyProtocol::SendStopListening() {
    stop_requested_ = true;
    if (listening_mode_ != kListeningModeAutoStop && listening_mode_ != kListeningModeRealtime) {
        // Manual mode worker is already waiting for stop; wake it by marking stop requested.
        // Auto mode also honors this flag for button/press-to-stop cases.
    }
}

void ThirdPartyProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    AbortCurrentWork(false);
}

void ThirdPartyProtocol::StartWorker() {
    if (worker_task_ != nullptr) {
        return;
    }
    abort_requested_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "third_party_ai_worker", 1024 * 16, this, 5, &worker_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create third-party AI worker");
        worker_task_ = nullptr;
    }
}

void ThirdPartyProtocol::StopWorker() {
    abort_requested_ = true;
    if (worker_task_ == nullptr) {
        return;
    }

    TaskHandle_t task = worker_task_;
    worker_task_ = nullptr;
    // Give the task a short time to observe abort_requested_ and exit.
    for (int i = 0; i < 20 && eTaskGetState(task) != eDeleted && eTaskGetState(task) != eInvalid; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (eTaskGetState(task) != eDeleted && eTaskGetState(task) != eInvalid) {
        vTaskDelete(task);
    }
}

void ThirdPartyProtocol::AbortCurrentWork(bool notify_closed) {
    abort_requested_ = true;

    const bool current_worker =
        worker_task_ != nullptr && worker_task_ == xTaskGetCurrentTaskHandle();
    if (current_worker) {
        worker_task_ = nullptr;
    } else {
        StopWorker();
    }

    Application::GetInstance().GetAudioService().ResetDecoder();
    if (notify_closed && channel_opened_.exchange(false) && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

void ThirdPartyProtocol::EmitJson(const char* type, const char* key1, const char* value1,
                                  const char* key2, const char* value2) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }
    cJSON_AddStringToObject(root, "type", type);
    if (key1 != nullptr && value1 != nullptr) {
        cJSON_AddStringToObject(root, key1, value1);
    }
    if (key2 != nullptr && value2 != nullptr) {
        cJSON_AddStringToObject(root, key2, value2);
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}

bool ThirdPartyProtocol::DecodeOpusPacketsToWav(
    const std::vector<std::unique_ptr<AudioStreamPacket>>& packets, std::vector<uint8_t>& wav) {
    if (packets.empty()) {
        return false;
    }

    int sample_rate = packets.front()->sample_rate > 0 ? packets.front()->sample_rate : 16000;
    int frame_duration = packets.front()->frame_duration > 0 ? packets.front()->frame_duration : 60;

    esp_opus_dec_cfg_t opus_cfg = {
        .sample_rate = static_cast<uint32_t>(sample_rate),
        .channel = ESP_AUDIO_MONO,
        .frame_duration = FrameDurationEnum(frame_duration),
        .self_delimited = false,
    };
    void* decoder = nullptr;
    auto ret = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &decoder);
    if (decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to open OPUS decoder for ASR input, error code: %d", ret);
        return false;
    }

    std::vector<int16_t> pcm;
    for (const auto& packet : packets) {
        std::vector<int16_t> decoded(sample_rate * frame_duration / 1000);
        esp_audio_dec_in_raw_t raw = {
            .buffer = packet->payload.data(),
            .len = static_cast<uint32_t>(packet->payload.size()),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer = reinterpret_cast<uint8_t*>(decoded.data()),
            .len = static_cast<uint32_t>(decoded.size() * sizeof(int16_t)),
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};
        ret = esp_opus_dec_decode(decoder, &raw, &out, &info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "OPUS decode failed, error code: %d", ret);
            esp_opus_dec_close(decoder);
            return false;
        }
        decoded.resize(out.decoded_size / sizeof(int16_t));
        pcm.insert(pcm.end(), decoded.begin(), decoded.end());
    }
    esp_opus_dec_close(decoder);

    if (pcm.empty()) {
        return false;
    }

    uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    uint32_t byte_rate = static_cast<uint32_t>(sample_rate) * 1 * 2;
    uint16_t block_align = 1 * 2;
    uint16_t bits_per_sample = 16;

    wav.reserve(44 + data_size);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    WriteU32(wav, 36 + data_size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    WriteU32(wav, 16);
    WriteU16(wav, 1);  // PCM
    WriteU16(wav, 1);  // mono
    WriteU32(wav, sample_rate);
    WriteU32(wav, byte_rate);
    WriteU16(wav, block_align);
    WriteU16(wav, bits_per_sample);

    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    WriteU32(wav, data_size);
    const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pcm_bytes, pcm_bytes + data_size);
    return true;
}

void ThirdPartyProtocol::ProcessConversation() {
    if (abort_requested_.load()) {
        return;
    }

    std::vector<std::unique_ptr<AudioStreamPacket>> packets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packets = std::move(recorded_packets_);
        recorded_packets_.clear();
    }

    if (packets.empty() || abort_requested_.load()) {
        ESP_LOGW(TAG, "No audio captured for third-party pipeline");
        AbortCurrentWork(true);
        return;
    }

    std::vector<uint8_t> wav;
    if (!DecodeOpusPacketsToWav(packets, wav)) {
        ESP_LOGE(TAG, "Failed to create WAV for ASR");
        AbortCurrentWork(true);
        return;
    }

    auto recognition = asr_->Recognize(wav);
    if (!recognition || recognition->text.empty()) {
        ESP_LOGE(TAG, "Third-party ASR failed");
        AbortCurrentWork(true);
        return;
    }
    if (abort_requested_.load()) {
        return;
    }

    EmitJson("stt", "text", recognition->text.c_str());
    EmitJson("llm", "emotion", recognition->emotion.empty() ? "neutral" : recognition->emotion.c_str());

    ChatModel::Message system_message;
    system_message.role = "system";
    system_message.content = "你是一个语音助手。请用简洁自然的中文回答用户。";

    ChatModel::Message user_message;
    user_message.role = "user";
    user_message.content = recognition->text;

    std::vector<ChatModel::Message> messages = {std::move(system_message), std::move(user_message)};
    auto reply = llm_->Chat(messages);
    if (!reply || reply->empty()) {
        ESP_LOGE(TAG, "Third-party LLM failed");
        AbortCurrentWork(true);
        return;
    }
    if (abort_requested_.load()) {
        return;
    }

    auto speech = tts_->Synthesize(*reply);
    if (!speech || !speech->ok || speech->pcm.empty()) {
        ESP_LOGE(TAG, "Third-party TTS failed: %s",
                 speech ? speech->error_message.c_str() : "no result");
        AbortCurrentWork(true);
        return;
    }
    if (abort_requested_.load()) {
        return;
    }

    EmitJson("tts", "state", "start");
    EmitJson("tts", "state", "sentence_start", "text", reply->c_str());
    auto& audio = Application::GetInstance().GetAudioService();
    if (!audio.PushPcmToPlaybackQueue(speech->pcm, speech->sample_rate, 0, 0, true)) {
        ESP_LOGE(TAG, "Failed to queue third-party TTS audio");
        AbortCurrentWork(true);
        return;
    }

    // Wait until the locally queued PCM has been played. Application changes to
    // listening/speaking state from the tts stop event, so keep the stop event
    // until playback is actually drained.
    while (!audio.IsPlaybackIdle() && !abort_requested_.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    EmitJson("tts", "state", "stop");
}

void ThirdPartyProtocol::WorkerEntry(void* self) {
    auto* protocol = static_cast<ThirdPartyProtocol*>(self);

    constexpr int kPollMs = 50;
    constexpr int kSilenceCount = 18;  // ~900 ms of trailing silence for auto-stop mode.
    int silence = 0;
    bool saw_voice = false;
    bool stopped = false;

    while (!protocol->abort_requested_.load() && !stopped) {
        bool voice = Application::GetInstance().IsVoiceDetected();
        bool manual = protocol->listening_mode_ == kListeningModeManualStop;

        if (manual) {
            if (protocol->stop_requested_.load()) {
                stopped = true;
            }
            vTaskDelay(pdMS_TO_TICKS(kPollMs));
            continue;
        }

        if (!saw_voice) {
            // Auto/realtime mode waits for voice activity before ending. This keeps
            // silence at the beginning of the listening window from immediately firing ASR.
            if (voice) {
                saw_voice = true;
                silence = 0;
            }
        } else {
            if (voice) {
                silence = 0;
            } else if (++silence >= kSilenceCount || protocol->stop_requested_.load()) {
                stopped = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }

    if (!protocol->abort_requested_.load()) {
        protocol->ProcessConversation();
    }

    protocol->worker_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool ThirdPartyProtocol::SendText(const std::string& text) {
    (void)text;
    return true;
}
