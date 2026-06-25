/**
 * lingxin_sdk_protocol.h - LingXin SDK Protocol for v2.6.6
 *
 * Wraps LingXin SDK's voice_chat API (chat_api.h) as a Protocol implementation.
 * SDK-managed WebSocket, state machine, and audio lifecycle.
 */

#ifndef LINGXIN_SDK_PROTOCOL_H
#define LINGXIN_SDK_PROTOCOL_H

#include "protocol.h"
#include "chat_api.h"

class LingxinSdkProtocol : public Protocol {
public:
    LingxinSdkProtocol();
    ~LingxinSdkProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

    static void SdkLifeCycleHandler(ChatLifeCycleEvent event, void *payload);
    static LingxinSdkProtocol* GetInstance() { return instance_; }
    bool IsInitialized() const { return sdk_initialized_; }

    /** Called from bridge when first downlink packet is queued. */
    void OnDownlinkStarted();

    /** True when SDK internal recorder (upload state) has been reached */
    bool IsRecorderOpen() const { return audio_channel_opened_; }

    /** True when SDK is paused (exit_chat with keep-WS-alive), waiting for next round */
    bool IsPaused() const { return chat_paused_; }

    /** Set the speaker name for voiceprint identification result */
    void SetSpeaker(const std::string& speaker, const std::string& message);

    /** Feed buffered PCM audio into SDK record ring buffer */
    void FeedBufferedAudio(const std::vector<int16_t>& pcm_data);

    /** Finish the SDK input after feeding buffered PCM audio */
    void FinishBufferedAudioInput();

private:
    bool SendText(const std::string& text) override;

    bool sdk_initialized_ = false;
    bool audio_channel_opened_ = false;
    bool chat_session_active_ = false;
    bool pending_outputing_ = false;
    bool close_requested_ = false;
    bool chat_paused_ = false;
    std::string current_wake_word_;
    std::string mcp_message_buffer_;
    std::string chat_mode_;
    std::string flow_control_strategy_;
    int flow_control_max_size_ = 32;
    int flow_control_space_time_ms_ = 120;
    std::string biz_speaker_;
    std::string biz_message_;
    std::string saved_task_id_;

    void ClearChatSessionFlags();
    void LoadRuntimeConfig();
    void ApplyStartNewChatProps(StartNewChatProps& props);
    void ApplyChatPhase(ChatPhaseCode phase);
    bool IsConversationDeviceState() const;

    static LingxinSdkProtocol* instance_;

    static char* GetAppId();
    static char* GetLicense();
    static char* GetSn();
    static char* GetAppCode();
    static char* GetDeviceCode();
    static char* GetBizParameter();
    static char* GetFlowControlParameter();

    void HandleChatPhaseChange(ChatPhaseCode phase);
    void HandleTextOut(char *text);
    void HandleExit(ExitCode exit_code, char *reason);
    void HandlePlayEnd();
    void HandleError();
};

#endif /* LINGXIN_SDK_PROTOCOL_H */
