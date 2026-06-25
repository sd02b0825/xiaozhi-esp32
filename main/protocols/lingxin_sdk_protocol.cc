/**
 * lingxin_sdk_protocol.cc - LingXin SDK Protocol implementation
 */

#include "sdkconfig.h"

#if CONFIG_LINGXIN_SDK_ENABLE

#include "lingxin_sdk_protocol.h"
#include "lingxin_sdk_bridge.h"
#include "audio_service.h"
#include "application.h"
#include "device_state.h"
#include "boards/common/board.h"
#include "settings.h"
extern "C" {
#include "chat_state_machine_event.h"
#include "audio_buffer_play.h"
}
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <string>

static const char *TAG = "LingxinSdkProtocol";

LingxinSdkProtocol* LingxinSdkProtocol::instance_ = nullptr;

namespace {

std::string GetLingxinConfigString(const char* nvs_key, const char* kconfig_default) {
    Settings settings("lingxin", false);
    std::string value = settings.GetString(nvs_key, "");
    if (!value.empty()) {
        return value;
    }
    if (kconfig_default != nullptr && kconfig_default[0] != '\0') {
        return kconfig_default;
    }
    return "";
}

int GetLingxinConfigInt(const char* nvs_key, int kconfig_default) {
    Settings settings("lingxin", false);
    return settings.GetInt(nvs_key, kconfig_default);
}

}  // namespace

/* ---- Auth / config callbacks (called by SDK) ---- */

char* LingxinSdkProtocol::GetAppId() {
    static std::string value;
    if (value.empty()) {
        value = GetLingxinConfigString("app_id", CONFIG_LINGXIN_APP_ID);
    }
    return const_cast<char*>(value.c_str());
}

char* LingxinSdkProtocol::GetLicense() {
    static std::string value;
    if (value.empty()) {
        value = GetLingxinConfigString("app_key", CONFIG_LINGXIN_APP_KEY);
    }
    return const_cast<char*>(value.c_str());
}

char* LingxinSdkProtocol::GetSn() {
    static std::string value;
    if (value.empty()) {
        value = GetLingxinConfigString("sn", CONFIG_LINGXIN_SN);
        if (value.empty()) {
            value = Board::GetInstance().GetUuid();
        }
    }
    return const_cast<char*>(value.c_str());
}

char* LingxinSdkProtocol::GetAppCode() {
    static std::string value;
    if (value.empty()) {
        value = GetLingxinConfigString("ai_app_code", CONFIG_LINGXIN_AI_APP_CODE);
    }
    return const_cast<char*>(value.c_str());
}

char* LingxinSdkProtocol::GetDeviceCode() {
    static std::string value;
    if (value.empty()) {
        value = GetLingxinConfigString("device_code", CONFIG_LINGXIN_DEVICE_CODE);
    }
    return const_cast<char*>(value.c_str());
}

char* LingxinSdkProtocol::GetBizParameter() {
    std::string device_code = GetLingxinConfigString("device_code", CONFIG_LINGXIN_DEVICE_CODE);
    auto* self = GetInstance();
    std::string speaker = self ? self->biz_speaker_ : "";
    std::string message = self ? self->biz_message_ : "";
    

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "GetBizParameter: cJSON_CreateObject failed");
        static std::string empty_json = "{}";
        return const_cast<char*>(empty_json.c_str());
    }
    cJSON_AddStringToObject(root, "device_code", device_code.c_str());
    cJSON* conversation_template_params = cJSON_AddArrayToObject(root, "conversation_template_params");
    if (conversation_template_params) {
        cJSON* param1 = cJSON_CreateObject();
        if (param1) {
            cJSON_AddStringToObject(param1, "name", "speaker");
            cJSON_AddStringToObject(param1, "value", speaker.c_str());
            cJSON_AddItemToArray(conversation_template_params, param1);
        }
        cJSON* param2 = cJSON_CreateObject();
        if (param2) {
            cJSON_AddStringToObject(param2, "name", "message");
            cJSON_AddStringToObject(param2, "value", message.c_str());
            cJSON_AddItemToArray(conversation_template_params, param2);
        }
    }

    static std::string json;
    char* printed = cJSON_PrintUnformatted(root);
    if (printed) {
        json = printed;
        cJSON_free(printed);
    } else {
        json = "{}";
    }
    ESP_LOGI(TAG, "GetBizParameter: %s", json.c_str());
    cJSON_Delete(root);
    return const_cast<char*>(json.c_str());
}

char* LingxinSdkProtocol::GetFlowControlParameter() {
    static std::string json;
    if (!json.empty()) {
        return const_cast<char*>(json.c_str());
    }

    auto* self = GetInstance();
    if (!self) {
        return const_cast<char*>("{}");
    }

    cJSON* root = cJSON_CreateObject();
    if (!self->flow_control_strategy_.empty() && self->flow_control_strategy_ != "none") {
        cJSON_AddStringToObject(root, "flow_control_strategy", self->flow_control_strategy_.c_str());
        if (self->flow_control_strategy_ == "fixed_byte_rate") {
            cJSON_AddNumberToObject(root, "max_size", self->flow_control_max_size_);
        } else if (self->flow_control_strategy_ == "fixed_time_interval") {
            cJSON_AddNumberToObject(root, "space_time", self->flow_control_space_time_ms_);
        }
    }
    char* printed = cJSON_PrintUnformatted(root);
    json = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return const_cast<char*>(json.c_str());
}

void LingxinSdkProtocol::LoadRuntimeConfig() {
    chat_mode_ = GetLingxinConfigString("mode", CONFIG_LINGXIN_CHAT_MODE);
    std::transform(chat_mode_.begin(), chat_mode_.end(), chat_mode_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (chat_mode_ != "voice" && chat_mode_ != "cloud_vad" &&
        chat_mode_ != "text_to_voice" && chat_mode_ != "full_duplex") {
        ESP_LOGW(TAG, "Unsupported chat mode %s, fallback to cloud_vad", chat_mode_.c_str());
        chat_mode_ = "cloud_vad";
    }

    flow_control_strategy_ = GetLingxinConfigString("fc_strategy", CONFIG_LINGXIN_FLOW_CONTROL_STRATEGY);
    flow_control_max_size_ = GetLingxinConfigInt("fc_max_size", CONFIG_LINGXIN_FLOW_CONTROL_MAX_SIZE);
    flow_control_space_time_ms_ = GetLingxinConfigInt("fc_sp_time_ms", CONFIG_LINGXIN_FLOW_CONTROL_SPACE_TIME_MS);

    if (flow_control_max_size_ != 32 && flow_control_max_size_ != 64 &&
        flow_control_max_size_ != 128 && flow_control_max_size_ != 256) {
        flow_control_max_size_ = 32;
    }
    if (flow_control_space_time_ms_ < 80 || flow_control_space_time_ms_ > 1000) {
        flow_control_space_time_ms_ = 120;
    }

    ESP_LOGI(TAG, "Runtime config mode=%s fc=%s", chat_mode_.c_str(), flow_control_strategy_.c_str());
}

void LingxinSdkProtocol::ApplyStartNewChatProps(StartNewChatProps& props) {
    static std::string task_storage;

    if (chat_mode_ == "voice") {
        task_storage = "chat";
    } else if (chat_mode_ == "text_to_voice") {
        task_storage = "chat_vad";
    } else if (chat_mode_ == "full_duplex") {
        ESP_LOGW(TAG, "full_duplex not confirmed on SDK path, downgrade to chat_vad");
        task_storage = "chat_vad";
    } else {
        task_storage = "chat_vad";
    }
    props.task = task_storage.empty() ? nullptr : const_cast<char*>(task_storage.c_str());
}

bool LingxinSdkProtocol::IsConversationDeviceState() const {
    auto state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateConnecting || state == kDeviceStateListening ||
           state == kDeviceStateSpeaking;
}

void LingxinSdkProtocol::ApplyChatPhase(ChatPhaseCode phase) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    switch (phase) {
    case CHAT_PHASE_STANDBY:
        if (chat_session_active_ && IsConversationDeviceState()) {
            pending_outputing_ = false;
        }
        break;

    case CHAT_PHASE_STARTING:
        if (chat_session_active_ && state != kDeviceStateActivating &&
            state != kDeviceStateWifiConfiguring && state != kDeviceStateAudioTesting &&
            state != kDeviceStateUpgrading) {
            app.SetDeviceState(kDeviceStateConnecting);
        }
        break;

    case CHAT_PHASE_INPUTING:
        audio_channel_opened_ = true;
        pending_outputing_ = false;
        if (on_audio_channel_opened_) {
            on_audio_channel_opened_();
        }
        if (chat_session_active_ && state != kDeviceStateActivating &&
            state != kDeviceStateWifiConfiguring && state != kDeviceStateAudioTesting) {
            app.SetDeviceState(kDeviceStateListening);
        }
        break;

    case CHAT_PHASE_THINKING:
        break;

    case CHAT_PHASE_OUTPUTING:
        pending_outputing_ = true;
        if (audio_service_is_playback_busy()) {
            app.SetDeviceState(kDeviceStateSpeaking);
        }
        break;

    case CHAT_PHASE_INTERRUPTING:
        pending_outputing_ = false;
        audio_service_reset_decoder();
        if (chat_session_active_ && IsConversationDeviceState()) {
            app.SetDeviceState(kDeviceStateListening);
        }
        break;

    case CHAT_PHASE_EXITING: {
        pending_outputing_ = false;
        bool notify_close = chat_session_active_ || audio_channel_opened_;
        ClearChatSessionFlags();
        if (notify_close && on_audio_channel_closed_ && !chat_paused_) {
            on_audio_channel_closed_();
        }
        break;
    }

    default:
        break;
    }
}

void LingxinSdkProtocol::OnDownlinkStarted() {
    if (!pending_outputing_) {
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateSpeaking) {
        app.SetDeviceState(kDeviceStateSpeaking);
    }
}

/* ---- SDK Lifecycle callback (runs on SDK thread) ---- */

void LingxinSdkProtocol::SdkLifeCycleHandler(ChatLifeCycleEvent event, void *payload) {
    auto *self = GetInstance();
    if (!self) {
        ESP_LOGE(TAG, "SdkLifeCycleHandler called but no instance");
        return;
    }

    switch (event) {
    case CHAT_LIFE_CYCLE_EVENT_CHAT_PHASE_CHANGE: {
        if (payload) {
            ChatPhaseChangePayload *phase_payload = (ChatPhaseChangePayload *)payload;
            ChatPhaseCode phase = phase_payload->phase_code;
            ESP_LOGI(TAG, "SDK ChatPhase: %d (scheduling to main thread)", phase);
            Application::GetInstance().Schedule([self, phase]() {
                self->HandleChatPhaseChange(phase);
            });
        }
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_EXIT: {
        if (payload) {
            ExitPayload *exit_payload = (ExitPayload *)payload;
            ExitCode exit_code = exit_payload->exit_code;
            char *reason_copy = exit_payload->reason ? strdup(exit_payload->reason) : nullptr;
            Application::GetInstance().Schedule([self, exit_code, reason_copy]() {
                self->HandleExit(exit_code, reason_copy);
            });
        }
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_TEXT_OUT: {
        if (payload) {
            char *text_copy = strdup((const char *)payload);
            Application::GetInstance().Schedule([self, text_copy]() {
                self->HandleTextOut(text_copy);
            });
        }
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_PLAY_END: {
        Application::GetInstance().Schedule([self]() {
            self->HandlePlayEnd();
        });
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_ERROR: {
        Application::GetInstance().Schedule([self]() {
            self->HandleError();
        });
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_SCHEDULE_EMIT:
        break;
    }
}

void LingxinSdkProtocol::HandleChatPhaseChange(ChatPhaseCode phase) {
    ESP_LOGI(TAG, "HandleChatPhaseChange: %d", phase);
    ApplyChatPhase(phase);
}

void LingxinSdkProtocol::HandleTextOut(char *text) {
    if (text && on_incoming_json_) {
        cJSON *root = cJSON_Parse(text);
        if (root) {
            auto header_obj = cJSON_GetObjectItem(root, "header");
            if (cJSON_IsObject(header_obj)) {
                auto task_id = cJSON_GetObjectItem(header_obj, "task_id");
                if (cJSON_IsString(task_id) && task_id->valuestring != nullptr) {
                    saved_task_id_ = task_id->valuestring;
                    ESP_LOGI(TAG, "HandleTextOut: captured task_id=%s", saved_task_id_.c_str());
                }
            }
            cJSON *action = cJSON_IsObject(header_obj) ? cJSON_GetObjectItem(header_obj, "action") : nullptr;
            if (!cJSON_IsString(action)) {
                if (!cJSON_IsObject(header_obj)) {
                    header_obj = cJSON_CreateObject();
                    cJSON_AddItemToObject(root, "header", header_obj);
                }
                cJSON_AddStringToObject(header_obj, "action", "text_output");
            }
            on_incoming_json_(root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "HandleTextOut: JSON parse failed, raw text: %.128s", text);
        }
    }
    free(text);
}

void LingxinSdkProtocol::HandleExit(ExitCode exit_code, char *reason) {
    ESP_LOGI(TAG, "HandleExit: code=%d, reason=%s, paused=%d", exit_code, reason ? reason : "null", chat_paused_);

    if (chat_paused_ && exit_code == EXIT_REASON_USER_INITIATED) {
        pending_outputing_ = false;
        chat_session_active_ = false;
        audio_channel_opened_ = false;
        close_requested_ = false;
        ESP_LOGI(TAG, "HandleExit: paused, keeping saved_task_id=%s for next round", saved_task_id_.c_str());
        if (on_audio_channel_closed_) {
            on_audio_channel_closed_();
        }
        free(reason);
        return;
    }

    pending_outputing_ = false;
    bool notify_close = chat_session_active_ || audio_channel_opened_;
    ClearChatSessionFlags();
    chat_paused_ = false;
    if (notify_close && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    free(reason);
}

void LingxinSdkProtocol::HandlePlayEnd() {
    ESP_LOGI(TAG, "HandlePlayEnd");
    pending_outputing_ = false;
    auto& app = Application::GetInstance();
    app.GetAudioService().WaitForPlaybackQueueEmpty();

    if (chat_mode_ == "voice" && IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "HandlePlayEnd: pausing SDK (keep WS alive, reuse task_id)");
        chat_paused_ = true;
        ExitChatProps exit_props = get_exit_chat_default_props();
        exit_props.disable_close_ws_immediately = true;
        exit_chat(&exit_props);
        return;
    }

    if (audio_service_is_sdk_uplink_active()) {
        return;
    }
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.SetDeviceState(kDeviceStateListening);
    }
}

void LingxinSdkProtocol::HandleError() {
    ESP_LOGE(TAG, "HandleError");
    if (on_network_error_) {
        on_network_error_("SDK internal error");
    }
}

LingxinSdkProtocol::LingxinSdkProtocol() {
    instance_ = this;
    LoadRuntimeConfig();
}

LingxinSdkProtocol::~LingxinSdkProtocol() {
    instance_ = nullptr;
}

void LingxinSdkProtocol::ClearChatSessionFlags() {
    chat_session_active_ = false;
    audio_channel_opened_ = false;
    pending_outputing_ = false;
    close_requested_ = false;
}

bool LingxinSdkProtocol::Start() {
    ESP_LOGI(TAG, "Initializing LingXin SDK voice_chat");
    // LoadRuntimeConfig();

    VoiceChatInitProps props = get_voice_chat_init_default_props();
    props.auth_app_id_get_func = GetAppId;
    props.auth_license_get_func = GetLicense;
    props.auth_sn_get_func = GetSn;
    props.auth_app_code_get_func = GetAppCode;
    props.device_code_get_func = GetDeviceCode;
    props.chat_biz_parameter_get_func = GetBizParameter;
    props.chat_flow_control_parameter_get_func = GetFlowControlParameter;
    props.chat_life_cycle_event_listener = SdkLifeCycleHandler;

    props.send_uni_size = 0;
    props.send_cbuf_scale = 0;
    props.welcome_audio_path = NULL;
    props.terminate_audio_path = NULL;
    props.continue_audio_path = NULL;
    props.is_schedule_task_on = 0;
    props.is_log_upload_on = 0;

    int ret = voice_chat_init(&props);
    if (ret != 0) {
        ESP_LOGE(TAG, "voice_chat_init failed: %d", ret);
        return false;
    }

    sdk_initialized_ = true;
    ESP_LOGI(TAG, "LingXin SDK initialized successfully");
    return true;
}

bool LingxinSdkProtocol::OpenAudioChannel() {
    if (!sdk_initialized_) {
        ESP_LOGE(TAG, "SDK not initialized");
        return false;
    }

    if (chat_session_active_) {
        ESP_LOGD(TAG, "Chat session already active, skip start_new_chat");
        return true;
    }

    ESP_LOGI(TAG, "Opening audio channel (start_new_chat), paused=%d, saved_task_id=%s",
             chat_paused_, saved_task_id_.c_str());
    close_requested_ = false;

    StartNewChatProps start_props = get_start_new_chat_default_props();
    start_props.disable_welcome_audio = true;
    start_props.single_round = false;
    start_props.play_prologue = false;
    ApplyStartNewChatProps(start_props);

    if (chat_paused_ && !saved_task_id_.empty()) {
        start_props.task_id = const_cast<char*>(saved_task_id_.c_str());
        ESP_LOGI(TAG, "Resuming with saved task_id=%s", saved_task_id_.c_str());
    }

    int ret = start_new_chat(&start_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "start_new_chat failed: %d", ret);
        return false;
    }

    chat_session_active_ = true;
    chat_paused_ = false;
    return true;
}

void LingxinSdkProtocol::CloseAudioChannel(bool send_goodbye) {
    ESP_LOGI(TAG, "Closing audio channel (exit_chat), send_goodbye=%d", send_goodbye);

    if (!sdk_initialized_) {
        return;
    }
    if (close_requested_) {
        ESP_LOGD(TAG, "CloseAudioChannel already requested, skip duplicate exit_chat");
        return;
    }
    close_requested_ = true;

    ExitChatProps exit_props = get_exit_chat_default_props();
    // send_goodbye=false → pause mode: keep WS alive, task_id reused on continue
    // send_goodbye=true  → full close: disconnect WS, new task_id on next session
    exit_props.disable_close_ws_immediately = !send_goodbye;

    int ret = exit_chat(&exit_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "exit_chat failed: %d", ret);
        bool notify_close = chat_session_active_ || audio_channel_opened_;
        ClearChatSessionFlags();
        if (notify_close && on_audio_channel_closed_) {
            on_audio_channel_closed_();
        }
        return;
    }
}

bool LingxinSdkProtocol::IsAudioChannelOpened() const {
    return chat_session_active_ || audio_channel_opened_;
}

bool LingxinSdkProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet) {
        ESP_LOGW(TAG, "SendAudio called with %u bytes but Lingxin SDK manages audio internally, discarding", packet->payload.size());
    }
    return true;
}

void LingxinSdkProtocol::SendWakeWordDetected(const std::string& wake_word) {
    current_wake_word_ = wake_word;
}

void LingxinSdkProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    if (!chat_session_active_ && !audio_channel_opened_) {
        ESP_LOGW(TAG, "SendStartListening without active session, opening channel");
        OpenAudioChannel();
    }
}

void LingxinSdkProtocol::SendStopListening() {
    StopChatRecordProps stop_props = {};
    stop_chat_record(&stop_props);
}

void LingxinSdkProtocol::FinishBufferedAudioInput() {
    if (!sdk_initialized_ || !chat_session_active_) {
        ESP_LOGW(TAG, "FinishBufferedAudioInput ignored: sdk_initialized=%d session_active=%d",
                 sdk_initialized_, chat_session_active_);
        return;
    }

    ESP_LOGI(TAG, "FinishBufferedAudioInput: stop SDK record after buffered PCM");
    StopChatRecordProps stop_props = {};
    int ret = stop_chat_record(&stop_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "FinishBufferedAudioInput: stop_chat_record failed: %d", ret);
    }
}

void LingxinSdkProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    ESP_LOGI(TAG, "SendAbortSpeaking: SDK terminate (Wakeup_Detected)");

    if (!sdk_initialized_ || !chat_session_active_) {
        return;
    }

    module_bufferPlay_terminate();
    state_machine_run_event(State_Event_Wakeup_Detected);
}

void LingxinSdkProtocol::SendMcpMessage(const std::string& message) {
    if (!sdk_initialized_) {
        return;
    }

    /* MCP tool result: send as user_input in current session context.
     * Note: SDK may start a new turn; same-task round-trip is not guaranteed.
     * mcp_message_buffer_ is a member variable and start_new_chat is synchronous,
     * so the pointer remains valid for the duration of the call. */
    mcp_message_buffer_ = message;

    StartNewChatProps start_props = get_start_new_chat_default_props();
    start_props.disable_welcome_audio = true;
    start_props.user_input = const_cast<char*>(mcp_message_buffer_.c_str());
    ApplyStartNewChatProps(start_props);

    int ret = start_new_chat(&start_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "start_new_chat with text input failed: %d", ret);
    } else {
        chat_session_active_ = true;
    }
}

bool LingxinSdkProtocol::SendText(const std::string& text) {
    (void)text;
    return true;
}

void LingxinSdkProtocol::SetSpeaker(const std::string& speaker, const std::string& message) {
    biz_speaker_ = speaker;
    biz_message_ = message;
    ESP_LOGI(TAG, "Speaker set to: %s, message: %s", speaker.c_str(), message.c_str());
}

void LingxinSdkProtocol::FeedBufferedAudio(const std::vector<int16_t>& pcm_data) {
    if (pcm_data.empty()) {
        return;
    }
    // Write in chunks to avoid ringbuf overflow (NOSPLIT requires contiguous space).
    // Each chunk is 8KB (4096 samples), well within the 256KB ringbuf capacity.
    constexpr size_t kChunkSamples = 4096;
    const int16_t* src = pcm_data.data();
    size_t remaining = pcm_data.size();
    size_t written = 0;
    int fail_count = 0;

    while (remaining > 0) {
        size_t chunk = std::min(remaining, kChunkSamples);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(src + written);
        size_t len = chunk * sizeof(int16_t);
        if (lingxin_record_write_pcm(data, len) == 0) {
            written += chunk;
            remaining -= chunk;
            fail_count = 0;
            // Let the SDK recorder task drain ringbuf into its send buffer during bulk replay.
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            fail_count++;
            ESP_LOGW(TAG, "FeedBufferedAudio: chunk write failed at %u/%u, retry %d",
                     (unsigned)written, (unsigned)pcm_data.size(), fail_count);
            if (fail_count > 50) {
                ESP_LOGE(TAG, "FeedBufferedAudio: giving up after %d retries at %u/%u",
                         fail_count, (unsigned)written, (unsigned)pcm_data.size());
                break;
            }
            // Wait for recorder thread to drain ringbuf space
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    ESP_LOGI(TAG, "FeedBufferedAudio: wrote %u/%u samples", (unsigned)written, (unsigned)pcm_data.size());
}

#endif  // CONFIG_LINGXIN_SDK_ENABLE
