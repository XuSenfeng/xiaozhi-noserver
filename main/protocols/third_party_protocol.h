#ifndef THIRD_PARTY_PROTOCOL_H
#define THIRD_PARTY_PROTOCOL_H

#include "protocol.h"
#include "services/ai_services.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class ThirdPartyProtocol : public Protocol {
public:
    ThirdPartyProtocol();
    ~ThirdPartyProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;

private:
    ThirdPartyAiConfig config_;
    NetworkInterface& network_;
    std::unique_ptr<SpeechRecognizer> asr_;
    std::unique_ptr<ChatModel> llm_;
    std::unique_ptr<SpeechSynthesizer> tts_;

    std::mutex mutex_;
    std::vector<std::unique_ptr<AudioStreamPacket>> recorded_packets_;

    std::atomic<bool> started_{false};
    std::atomic<bool> channel_opened_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t worker_task_ = nullptr;

    ListeningMode listening_mode_ = kListeningModeAutoStop;

    void StartWorker();
    void StopWorker();
    void ProcessConversation();
    bool DecodeOpusPacketsToWav(const std::vector<std::unique_ptr<AudioStreamPacket>>& packets,
                                std::vector<uint8_t>& wav);
    void EmitJson(const char* type, const char* key1 = nullptr, const char* value1 = nullptr,
                  const char* key2 = nullptr, const char* value2 = nullptr);
    void AbortCurrentWork(bool notify_closed);

    static void WorkerEntry(void* self);
    bool SendText(const std::string& text) override;
};

#endif  // THIRD_PARTY_PROTOCOL_H
