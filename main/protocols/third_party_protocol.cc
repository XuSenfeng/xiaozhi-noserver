#include "third_party_protocol.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"

#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <cJSON.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>
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

constexpr size_t kMaxPreVoiceRecordedPackets = 120;  // ~7.2s at 60 ms per packet
constexpr size_t kMaxVoiceRecordedPackets = 500;     // ~30s at 60 ms per packet

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

std::string TrimAsciiWhitespace(const std::string& text, size_t* begin = nullptr,
                                size_t* end = nullptr) {
    size_t b = 0;
    while (b < text.size() &&
           (text[b] == ' ' || text[b] == '\t' || text[b] == '\r' || text[b] == '\n')) {
        ++b;
    }
    size_t e = text.size();
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r' ||
                     text[e - 1] == '\n')) {
        --e;
    }
    if (begin != nullptr) {
        *begin = b;
    }
    if (end != nullptr) {
        *end = e;
    }
    return text.substr(b, e - b);
}

bool StartsWithAt(const std::string& text, size_t pos, const char* prefix) {
    size_t len = strlen(prefix);
    return pos + len <= text.size() && text.compare(pos, len, prefix) == 0;
}

bool IsLineEndingPunctuation(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    size_t size = text.size();
    return (size >= 3 &&
            (StartsWithAt(text, size - 3, "\u3002") || StartsWithAt(text, size - 3, "\uff01") ||
             StartsWithAt(text, size - 3, "\uff1f") || StartsWithAt(text, size - 3, "\uff1b") ||
             StartsWithAt(text, size - 3, "\u2026") || StartsWithAt(text, size - 3, "\uff1a") ||
             StartsWithAt(text, size - 3, "\uff0c") || StartsWithAt(text, size - 3, "\u3001"))) ||
           text.back() == '.' || text.back() == '!' || text.back() == '?' || text.back() == ';' ||
           text.back() == ':' || text.back() == ',';
}

size_t StripListMarker(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    while (pos < line.size() && line[pos] == '#') {
        ++pos;
    }
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }

    size_t marker_start = pos;
    if (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            ++pos;
        }
        bool followed_by_marker = pos >= line.size();
        if (!followed_by_marker &&
            (line[pos] == '.' || line[pos] == ')' || StartsWithAt(line, pos, "\u3001") ||
             StartsWithAt(line, pos, "\uff09"))) {
            followed_by_marker = true;
            pos += StartsWithAt(line, pos, "\u3001") || StartsWithAt(line, pos, "\uff09") ? 3 : 1;
        }
        if (followed_by_marker) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
                ++pos;
            }
            return pos;
        }
        return marker_start;
    }

    if (pos < line.size() && (line[pos] == '-' || line[pos] == '*') &&
        (pos + 1 >= line.size() || line[pos + 1] == ' ' || line[pos + 1] == '\t')) {
        pos = std::min(line.size(), pos + 2);
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        return pos;
    }
    return marker_start;
}

size_t SkipUrl(const std::string& text, size_t pos) {
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\r' &&
           text[pos] != '\n') {
        ++pos;
    }
    return pos;
}

std::string SanitizeReplyForTts(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    size_t line_start = 0;
    bool need_separator = false;
    bool pending_space = false;

    auto append_space = [&]() { pending_space = true; };

    auto flush_space = [&]() {
        if (!pending_space || out.empty()) {
            pending_space = false;
            return;
        }
        unsigned char previous = static_cast<unsigned char>(out.back());
        if (out.back() != ' ' && previous < 0x80 && !IsLineEndingPunctuation(out)) {
            out.push_back(' ');
        }
        pending_space = false;
    };

    auto append_plain = [&](const std::string& value) {
        flush_space();
        out += value;
    };

    auto append_separator = [&]() {
        pending_space = false;
        if (out.empty()) {
            return;
        }
        if (!IsLineEndingPunctuation(out)) {
            out += "\u3002";
        }
    };

    while (line_start <= text.size()) {
        size_t line_end = text.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = text.size();
        }
        std::string line = text.substr(line_start, line_end - line_start);

        size_t pos = StripListMarker(line);
        bool content_started = false;

        auto start_content = [&]() {
            if (content_started) {
                return;
            }
            content_started = true;
            if (need_separator) {
                append_separator();
                need_separator = false;
            }
        };

        while (pos < line.size()) {
            unsigned char c = static_cast<unsigned char>(line[pos]);

            if (c == ' ' || c == '\t' || c == '\r') {
                if (content_started) {
                    append_space();
                }
                ++pos;
                continue;
            }

            if (StartsWithAt(line, pos, "http://") || StartsWithAt(line, pos, "https://")) {
                pos = SkipUrl(line, pos);
                if (content_started) {
                    append_space();
                }
                continue;
            }
            if (StartsWithAt(line, pos, "www.")) {
                pos = SkipUrl(line, pos);
                if (content_started) {
                    append_space();
                }
                continue;
            }
            if (line[pos] == '[') {
                size_t close = line.find(']', pos + 1);
                if (close != std::string::npos) {
                    size_t url_start = close + 1;
                    if (url_start < line.size() && line[url_start] == '(') {
                        size_t url_end = line.find(')', url_start + 1);
                        if (url_end != std::string::npos) {
                            for (size_t i = pos + 1; i < close; ++i) {
                                char inner = line[i];
                                if (inner == '*' || inner == '_' || inner == '`') {
                                    continue;
                                }
                                start_content();
                                append_plain(std::string(1, inner));
                            }
                            pos = url_end + 1;
                            continue;
                        }
                    }
                }
            }

            bool ascii = c < 0x80;
            bool keep = true;
            if (ascii) {
                keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':' ||
                       c == '%' || c == '+';
                if (!keep) {
                    if (c == '-' || c == '_' || c == '*' || c == '#' || c == '`' || c == '~' ||
                        c == '|' || c == '\\' || c == '/' || c == '(' || c == ')' || c == '[' ||
                        c == ']' || c == '{' || c == '}' || c == '<' || c == '>' || c == '"' ||
                        c == '\'') {
                        if (content_started) {
                            append_space();
                        }
                    }
                }
            }

            if (keep) {
                if (ascii) {
                    start_content();
                    append_plain(std::string(1, static_cast<char>(c)));
                } else {
                    // Copy the whole UTF-8 sequence. This preserves Chinese characters/punctuation
                    // and avoids splitting a multi-byte character while stripping only ASCII
                    // Markdown noise.
                    size_t seq_end = pos;
                    while (seq_end < line.size() &&
                           (static_cast<unsigned char>(line[seq_end]) & 0x80)) {
                        ++seq_end;
                    }
                    start_content();
                    flush_space();
                    out.append(line, pos, seq_end - pos);
                    pos = seq_end;
                    continue;
                }
            }
            ++pos;
        }

        if (content_started) {
            flush_space();
            need_separator = true;
        }

        line_start = line_end + 1;
    }

    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

int Utf8SequenceLength(unsigned char c) {
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

bool IsReadableCodepoint(uint32_t codepoint) {
    if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= '0' && codepoint <= '9')) {
        return true;
    }
    return (codepoint >= 0x00C0 && codepoint <= 0x024F) ||  // Latin letters
           (codepoint >= 0x0370 && codepoint <= 0x03FF) ||  // Greek
           (codepoint >= 0x0400 && codepoint <= 0x04FF) ||  // Cyrillic
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||  // Japanese kana
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||  // CJK Extension A
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||  // CJK Unified Ideographs
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||  // Hangul syllables
           (codepoint >= 0xF900 && codepoint <= 0xFAFF);    // CJK Compatibility Ideographs
}

bool HasReadableText(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int length = Utf8SequenceLength(c);
        if (i + length > text.size()) {
            length = static_cast<int>(text.size() - i);
        }
        uint32_t codepoint = 0;
        if (length == 1) {
            codepoint = c;
        } else if (length == 2) {
            codepoint = static_cast<uint32_t>(c & 0x1F) << 6;
            codepoint |= static_cast<unsigned char>(text[i + 1]) & 0x3F;
        } else if (length == 3) {
            codepoint = static_cast<uint32_t>(c & 0x0F) << 12;
            codepoint |= (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6;
            codepoint |= static_cast<unsigned char>(text[i + 2]) & 0x3F;
        } else {
            codepoint = static_cast<uint32_t>(c & 0x07) << 18;
            codepoint |= (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12;
            codepoint |= (static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6;
            codepoint |= static_cast<unsigned char>(text[i + 3]) & 0x3F;
        }
        if (IsReadableCodepoint(codepoint)) {
            return true;
        }
        i += length;
    }
    return false;
}

std::vector<std::string> SplitReplyForTts(const std::string& text) {
    // Keep each TTS request small enough that DashScope/Qwen does not return a very
    // large WAV. A whole multi-sentence reply can easily become a 1+ MB WAV and exhaust
    // the ESP32-S3 heap while downloading and converting it to PCM.
    constexpr size_t kMaxTtsBytes = 60;
    constexpr size_t kMinTtsBytes = 30;  // About 10 Chinese characters.
    static const char* kPunctuation[] = {
        "\u3002",  // 。
        "\uff01",  // ！
        "\uff1f",  // ？
        "\uff1b",  // ；
        "\u2026",  // …
        "\uff1a",  // ：
        "\uff0c",  // ，
        "\u3001",  // 、
        "\n",     ".", "!", "?", ";", ":", ",",
    };

    std::vector<std::string> chunks;
    std::string chunk;

    auto push_chunk = [&](bool force) {
        std::string candidate = TrimAsciiWhitespace(chunk);
        if (candidate.empty() || !HasReadableText(candidate)) {
            chunk.clear();
            return;
        }
        // Prefer to accumulate until we have roughly 10 Chinese characters (30 UTF-8
        // bytes). This avoids submitting very short TTS requests that sound choppy.
        if (!force && candidate.size() < kMinTtsBytes) {
            return;
        }
        chunks.push_back(std::move(candidate));
        chunk.clear();
    };

    for (size_t i = 0; i < text.size();) {
        bool matched_boundary = false;
        size_t boundary_len = 0;
        for (const char* punctuation : kPunctuation) {
            size_t len = strlen(punctuation);
            if (text.compare(i, len, punctuation) == 0) {
                matched_boundary = true;
                boundary_len = len;
                break;
            }
        }

        if (matched_boundary) {
            chunk.append(text, i, boundary_len);
            i += boundary_len;
            push_chunk(false);
            continue;
        }

        unsigned char byte = static_cast<unsigned char>(text[i]);

        // Never split inside a UTF-8 multi-byte character: 0b10xxxxxx is a continuation byte.
        if ((byte & 0xC0) == 0x80) {
            chunk.push_back(static_cast<char>(byte));
            ++i;
            continue;
        }

        if (chunk.size() >= kMaxTtsBytes) {
            push_chunk(true);
        }
        chunk.push_back(static_cast<char>(byte));
        ++i;
    }

    push_chunk(true);
    return chunks;
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

ThirdPartyProtocol::~ThirdPartyProtocol() { StopWorker(); }

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

        // Listening can stay active for a long time before VAD triggers ASR (especially
        // silence/noise). Without a cap, recorded_packets_ grows unbounded on the internal
        // heap, eventually dropping free SRAM to near zero even after TTS PCM was moved to
        // PSRAM.
        const bool voice_detected = Application::GetInstance().IsVoiceDetected();
        const size_t max_packets =
            voice_detected ? kMaxVoiceRecordedPackets : kMaxPreVoiceRecordedPackets;
        if (recorded_packets_.size() > max_packets) {
            const size_t erase_count = recorded_packets_.size() - max_packets;
            recorded_packets_.erase(recorded_packets_.begin(),
                                    recorded_packets_.begin() + erase_count);
        }
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

    // A previous worker may be just finishing its final TTS stop event when Application
    // switches back from speaking to listening. Starting right after tts/stop is the normal
    // second-utterance path, but the old worker has not cleared worker_task_ yet. Don't drop
    // this listen request; ask the exiting worker to start the replacement immediately.
    if (worker_task_ != nullptr) {
        ESP_LOGI(TAG, "Listen start requested while previous worker is exiting; deferring restart");
        restart_worker_on_exit_ = true;
        return;
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
        restart_worker_on_exit_ = true;
        ESP_LOGW(TAG, "Third-party worker already running; restart requested on exit");
        return;
    }
    abort_requested_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "third_party_ai_worker", 1024 * 16, this, 5, &worker_task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "Failed to create third-party AI worker");
        worker_task_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Third-party AI worker started");
}

void ThirdPartyProtocol::StopWorker() {
    abort_requested_ = true;
    if (worker_task_ == nullptr) {
        return;
    }

    TaskHandle_t task = worker_task_;
    worker_task_ = nullptr;
    // Give the task a short time to observe abort_requested_ and exit.
    for (int i = 0; i < 20 && eTaskGetState(task) != eDeleted && eTaskGetState(task) != eInvalid;
         ++i) {
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

    // These buffers are only needed by ASR. Release them before running LLM/TTS,
    // otherwise the ASR WAV and all captured audio packets stay alive for the rest
    // of the conversation and contribute to OOM during HTTPS/TTS downloads.
    std::vector<uint8_t>().swap(wav);
    std::vector<std::unique_ptr<AudioStreamPacket>>().swap(packets);
    if (abort_requested_.load()) {
        return;
    }

    EmitJson("stt", "text", recognition->text.c_str());
    EmitJson("llm", "emotion",
             recognition->emotion.empty() ? "neutral" : recognition->emotion.c_str());

    ChatModel::Message system_message;
    system_message.role = "system";
    system_message.content =
        "你是一个只会用语音输出的中文语音助手。回答必须适合直接朗读：不要输出 Markdown、编号列表、"
        "项目符号、星号、反引号、# 号、括号、表格、代码、URL 或任何装饰符号。多个要点要自然连成"
        "完整短句，用逗号、句号等中文标点分隔，不要换行生成列表。";

    ChatModel::Message user_message;
    user_message.role = "user";
    user_message.content = recognition->text;

    std::vector<ToolDefinition> tools;
    auto mcp_tools = McpServer::GetInstance().GetAiTools();
    tools.reserve(mcp_tools.size());
    std::unordered_map<std::string, std::string> mcp_tool_names;
    for (const auto& tool : mcp_tools) {
        tools.push_back({
            .name = tool.name,
            .mcp_name = tool.mcp_name,
            .description = tool.description,
            .parameters_json = tool.parameters_json,
        });
        mcp_tool_names[tool.name] = tool.mcp_name;
    }

    std::vector<ChatModel::Message> messages = {std::move(system_message), std::move(user_message)};

    std::optional<std::string> reply;
    constexpr int kMaxLlmToolRounds = 4;
    for (int round = 0; round < kMaxLlmToolRounds; ++round) {
        auto response = llm_->Chat(messages, tools);
        if (!response) {
            ESP_LOGE(TAG, "Third-party LLM failed");
            AbortCurrentWork(true);
            return;
        }
        if (abort_requested_.load()) {
            return;
        }

        if (response->tool_calls.empty()) {
            if (!response->content || response->content->empty()) {
                ESP_LOGE(TAG, "Third-party LLM returned no content");
                AbortCurrentWork(true);
                return;
            }
            reply = *response->content;
            break;
        }

        // Persist the assistant tool-call turn before adding tool results.
        ChatModel::Message assistant_message;
        assistant_message.role = "assistant";
        assistant_message.content = response->content.value_or("");
        assistant_message.tool_calls = std::move(response->tool_calls);
        auto tool_calls_to_run = assistant_message.tool_calls;
        messages.push_back(std::move(assistant_message));

        for (const auto& tool_call : tool_calls_to_run) {
            if (abort_requested_.load()) {
                return;
            }

            CJsonUniquePtr arguments(cJSON_Parse(tool_call.arguments.c_str()));
            if (arguments == nullptr) {
                arguments.reset(cJSON_CreateObject());
            }

            std::string mcp_tool_name = tool_call.name;
            auto name_it = mcp_tool_names.find(tool_call.name);
            if (name_it != mcp_tool_names.end()) {
                mcp_tool_name = name_it->second;
            }

            auto tool_result = McpServer::GetInstance().CallAiTool(mcp_tool_name, arguments.get());
            std::string tool_content =
                tool_result ? *tool_result : "Tool failed: " + tool_result.error();

            ChatModel::Message tool_message;
            tool_message.role = "tool";
            tool_message.content = std::move(tool_content);
            tool_message.tool_call_id = tool_call.id;
            tool_message.tool_name = tool_call.name;
            messages.push_back(std::move(tool_message));
        }
    }

    if (!reply || reply->empty()) {
        ESP_LOGE(TAG, "Third-party LLM exceeded tool-call round limit");
        AbortCurrentWork(true);
        return;
    }
    if (abort_requested_.load()) {
        return;
    }

    ESP_LOGI(TAG, "Third-party LLM reply: %s", reply->c_str());

    std::string tts_text = SanitizeReplyForTts(*reply);
    ESP_LOGI(TAG, "Third-party TTS sanitized reply: %s", tts_text.c_str());

    // Before TTS download/playback, drop the LLM/tool/ASR working set. TTS downloads a whole
    // WAV into heap and then converts it to PCM; keeping conversation/tool history alive only
    // makes memory exhaustion more likely.
    recognition.reset();
    std::vector<ChatModel::Message>().swap(messages);
    std::vector<ToolDefinition>().swap(tools);
    mcp_tool_names.clear();
    mcp_tools.clear();

    std::vector<std::string> tts_chunks = SplitReplyForTts(tts_text);
    tts_text.clear();
    reply.reset();
    if (tts_chunks.empty()) {
        ESP_LOGE(TAG, "Third-party LLM returned empty TTS text");
        AbortCurrentWork(true);
        return;
    }

    ESP_LOGI(TAG, "Third-party TTS chunks: %d", static_cast<int>(tts_chunks.size()));
    for (size_t i = 0; i < tts_chunks.size(); ++i) {
        ESP_LOGI(TAG, "Third-party TTS chunk[%d]: %s", static_cast<int>(i), tts_chunks[i].c_str());
    }

    EmitJson("tts", "state", "start");
    auto& audio = Application::GetInstance().GetAudioService();

    auto next_speech = tts_->Synthesize(tts_chunks.front());
    if (!next_speech || !next_speech->ok || next_speech->pcm.empty()) {
        ESP_LOGE(TAG, "Third-party TTS failed: %s",
                 next_speech ? next_speech->error_message.c_str() : "no result");
        AbortCurrentWork(true);
        return;
    }

    for (size_t i = 0; i < tts_chunks.size(); ++i) {
        if (abort_requested_.load()) {
            return;
        }

        const std::string& chunk = tts_chunks[i];
        auto speech = std::move(next_speech);
        next_speech.reset();

        if (!speech || !speech->ok || speech->pcm.empty()) {
            ESP_LOGE(TAG, "Third-party TTS failed: %s",
                     speech ? speech->error_message.c_str() : "no result");
            AbortCurrentWork(true);
            return;
        }

        EmitJson("tts", "state", "sentence_start", "text", chunk.c_str());
        if (!audio.PushPcmToPlaybackQueue(std::move(speech->pcm), speech->sample_rate, 0, 0,
                                          true)) {
            ESP_LOGE(TAG, "Failed to queue third-party TTS audio");
            AbortCurrentWork(true);
            return;
        }

        // AudioService now takes ownership of the PCM vector when no resampling is needed.
        // Release the empty segment object before requesting the next segment.
        speech.reset();

        // Synthesize the next sentence while the current PCM is still queued or playing.
        // This keeps the next audio segment ready before playback drains and reduces the
        // pause between consecutive TTS chunks.
        if (i + 1 < tts_chunks.size()) {
            next_speech = tts_->Synthesize(tts_chunks[i + 1]);
            if (!next_speech || !next_speech->ok || next_speech->pcm.empty()) {
                ESP_LOGE(TAG, "Third-party TTS failed: %s",
                         next_speech ? next_speech->error_message.c_str() : "no result");
                AbortCurrentWork(true);
                return;
            }
        }

        if (i + 1 < tts_chunks.size()) {
            // Do not queue the next PCM chunk until the previous one has drained. We already
            // synthesized it above, so the next request does not add a download/decode gap.
            while (!audio.IsPlaybackIdle() && !abort_requested_.load()) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
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
    ESP_LOGD(TAG, "Third-party AI worker entered");

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
                ESP_LOGI(TAG, "Third-party AI worker saw voice, starting ASR window");
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

    // Application may have requested listening again while this worker was emitting tts/stop
    // or still returning from ProcessConversation. Start the next worker before deleting this
    // task; otherwise second and later utterances stop being recognized.
    protocol->worker_task_ = nullptr;
    if (protocol->restart_worker_on_exit_.exchange(false) && !protocol->abort_requested_.load()) {
        protocol->StartWorker();
    }

    vTaskDelete(nullptr);
}

bool ThirdPartyProtocol::SendText(const std::string& text) {
    (void)text;
    return true;
}
