/**
 * lingxin_sdk_protocol.cc - LingXin SDK Protocol implementation
 */

#include "sdkconfig.h"

#if CONFIG_LINGXIN_SDK_ENABLE

#include "lingxin_sdk_protocol.h"
#include "lingxin_sdk_bridge.h"
#include "lingxin_device_command.h"
#include "lingxin_device_command_listener.h"
#include "audio_service.h"
#include "application.h"
#include "device_state.h"
#include "boards/common/board.h"
#include "settings.h"
extern "C" {
#include "chat_state_machine_event.h"
#include "lingxin_alarm_trigger.h"
#include "audio_buffer_play.h"
}
#include "display.h"
#include "esp_log.h"
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

static const char *TAG = "LingxinSdkProtocol";

static bool IsVolumeDenialAgentText(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    return strstr(text, "无法") != nullptr && strstr(text, "音量") != nullptr;
}

static bool IsStandbyMisinterpretedMuteAgentText(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    /* 云端常把单说「关闭」误解为关声音，并在回复里顺带询问是否休息。 */
    const bool mentions_sound_off = strstr(text, "声音") != nullptr &&
                                    (strstr(text, "关掉") != nullptr || strstr(text, "关了") != nullptr ||
                                     strstr(text, "关上") != nullptr);
    return mentions_sound_off && strstr(text, "休息") != nullptr;
}

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
    
    static std::string json = "{\"device_code\":\"" + device_code + "\",\"conversation_template_params\":[{\"name\": \"speaker\", \"value\": \"张三\"}]}";
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
        lingxin_clear_volume_command_suppress();
        lingxin_clear_standby_after_playback();
        lingxin_set_standby_exit_in_progress(0);
        lingxin_device_command_clear_turn_state();
        audio_channel_opened_ = true;
        pending_outputing_ = false;
        if (on_audio_channel_opened_) {
            on_audio_channel_opened_();
        }
        if (lingxin_is_alarm_alert_turn()) {
            ESP_LOGI(TAG, "Skip listening state during alarm alert turn");
            break;
        }
        if (chat_session_active_ && state != kDeviceStateActivating &&
            state != kDeviceStateWifiConfiguring && state != kDeviceStateAudioTesting) {
            app.SetDeviceState(kDeviceStateListening);
        }
        break;

    case CHAT_PHASE_THINKING:
        /* Download init (phase 3): stop AFE before first MP3 packet to free CPU for decode. */
        audio_service_begin_downlink_playback();
        break;

    case CHAT_PHASE_OUTPUTING:
        pending_outputing_ = true;
        audio_service_begin_downlink_playback();
        if (!lingxin_should_suppress_cloud_tts() && audio_service_is_playback_busy()) {
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
        if (notify_close && on_audio_channel_closed_) {
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
    if (lingxin_should_suppress_cloud_tts()) {
        ESP_LOGI(TAG, "Skip speaking state: volume handled locally, cloud TTS suppressed");
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
    if (!text) {
        return;
    }

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        ESP_LOGW(TAG, "HandleTextOut: JSON parse failed, raw text: %.200s", text);
        free(text);
        return;
    }

    lingxin_device_command_note_text_output(root);
    if (lingxin_device_command_try_handle_pending_user_text()) {
        ESP_LOGI(TAG, "Handled standby from cached user text");
        audio_service_reset_decoder();
        cJSON_Delete(root);
        free(text);
        return;
    }

    const cJSON *type_field = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type_field) && type_field->valuestring != nullptr &&
        strcmp(type_field->valuestring, "agent_response_text") == 0) {
        const cJSON *result_field = cJSON_GetObjectItemCaseSensitive(root, "result");
        const cJSON *text_field = cJSON_GetObjectItemCaseSensitive(root, "text");
        const char *agent_text = cJSON_IsString(result_field) ? result_field->valuestring
                              : (cJSON_IsString(text_field) ? text_field->valuestring : nullptr);

        if (IsStandbyMisinterpretedMuteAgentText(agent_text) ||
            lingxin_device_command_is_mute_reply_for_standby(agent_text)) {
            ESP_LOGI(TAG, "Correct cloud mute misinterpretation for 关闭, entering standby");
            lingxin_set_suppress_cloud_tts(1);
            lingxin_set_standby_exit_in_progress(1);
            audio_service_reset_decoder();
            Application::GetInstance().EnterStandby();
            cJSON_Delete(root);
            free(text);
            return;
        }

        if (lingxin_device_command_is_standby_farewell_agent_text(agent_text)) {
            ESP_LOGI(TAG, "Standby farewell detected, enter standby after TTS");
            lingxin_mark_standby_after_playback();
        }

        if (lingxin_volume_command_handled_this_turn() && IsVolumeDenialAgentText(agent_text)) {
            const int target = lingxin_get_last_volume_command_target();
            lingxin_set_suppress_cloud_tts(1);
            ESP_LOGI(TAG, "Suppress conflicting volume denial TTS (target=%d)", target);
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                std::string message = "好的，已为您调整音量";
                if (target >= 0) {
                    message += "至" + std::to_string(target);
                }
                display->SetChatMessage("assistant", message.c_str());
            }
            cJSON_Delete(root);
            free(text);
            return;
        }
    }

    if (lingxin_device_command_try_handle_json(root)) {
        ESP_LOGI(TAG, "Handled device command from text_output");
        cJSON_Delete(root);
        free(text);
        return;
    }

    // INFO 级别便于确认云端实际下发的 text_output 结构
    ESP_LOGI(TAG, "text_output payload: %.300s", text);

    if (on_incoming_json_) {
        // SDK 传入的是 payload 对象，需包装成 application 期望的 header+payload 结构
        cJSON *message = cJSON_CreateObject();
        cJSON *header_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(header_obj, "action", "text_output");
        cJSON_AddItemToObject(message, "header", header_obj);
        cJSON_AddItemToObject(message, "payload", cJSON_Duplicate(root, 1));
        on_incoming_json_(message);
        cJSON_Delete(message);
    }

    cJSON_Delete(root);
    free(text);
}

void LingxinSdkProtocol::HandleExit(ExitCode exit_code, char *reason) {
    ESP_LOGI(TAG, "HandleExit: code=%d, reason=%s", exit_code, reason ? reason : "null");
    pending_outputing_ = false;
    lingxin_set_alarm_alert_turn(0);
    bool notify_close = chat_session_active_ || audio_channel_opened_;
    ClearChatSessionFlags();
    if (notify_close && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    free(reason);
}

void LingxinSdkProtocol::HandlePlayEnd() {
    ESP_LOGI(TAG, "HandlePlayEnd");
    pending_outputing_ = false;
    const bool standby_after_playback = lingxin_standby_after_playback_pending() != 0;
    const bool alarm_alert_turn = lingxin_is_alarm_alert_turn() != 0;
    lingxin_clear_volume_command_suppress();
    lingxin_set_alarm_alert_turn(0);
    auto& app = Application::GetInstance();
    app.GetAudioService().WaitForPlaybackQueueEmpty();
    if (audio_service_is_sdk_uplink_active()) {
        return;
    }
    if (standby_after_playback) {
        lingxin_clear_standby_after_playback();
        lingxin_set_standby_exit_in_progress(1);
        ESP_LOGI(TAG, "Playback finished, entering standby");
        if (IsAudioChannelOpened()) {
            CloseAudioChannel();
        } else {
            app.SetDeviceState(kDeviceStateIdle);
            lingxin_set_standby_exit_in_progress(0);
        }
        return;
    }
    if (alarm_alert_turn) {
        ESP_LOGI(TAG, "Alarm alert playback finished, return to idle");
        if (IsAudioChannelOpened()) {
            CloseAudioChannel();
        } else if (app.GetDeviceState() == kDeviceStateSpeaking ||
                   app.GetDeviceState() == kDeviceStateListening) {
            app.SetDeviceState(kDeviceStateIdle);
        }
        return;
    }
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.SetDeviceState(kDeviceStateListening);
    }
}

void LingxinSdkProtocol::HandleError() {
    ESP_LOGE(TAG, "HandleError");
    lingxin_set_alarm_alert_turn(0);
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
    props.is_schedule_task_on = 1;
    props.is_log_upload_on = 0;

    int ret = voice_chat_init(&props);
    if (ret != 0) {
        ESP_LOGE(TAG, "voice_chat_init failed: %d", ret);
        return false;
    }

    sdk_initialized_ = true;
    lingxin_adapter_init_device_command_listener();
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

    ESP_LOGI(TAG, "Opening audio channel (start_new_chat)");

    StartNewChatProps start_props = get_start_new_chat_default_props();
    start_props.disable_welcome_audio = true;
    start_props.single_round = false;
    start_props.play_prologue = true;   
    ApplyStartNewChatProps(start_props);

    int ret = start_new_chat(&start_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "start_new_chat failed: %d", ret);
        return false;
    }

    chat_session_active_ = true;
    return true;
}

void LingxinSdkProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    ESP_LOGI(TAG, "Closing audio channel (exit_chat)");

    if (!sdk_initialized_) {
        return;
    }

    ExitChatProps exit_props = get_exit_chat_default_props();
    exit_props.disable_close_ws_immediately = false;

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

void LingxinSdkProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (lingxin_standby_exit_in_progress()) {
        ESP_LOGI(TAG, "Skip SendAbortSpeaking during standby exit");
        return;
    }
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

void LingxinSdkProtocol::RequestAlarmCloudTts(const std::string& message, const char* schedule_task_id) {
    if (!sdk_initialized_) {
        ESP_LOGW(TAG, "RequestAlarmCloudTts: SDK not initialized");
        return;
    }

    if (schedule_task_id != nullptr && schedule_task_id[0] != '\0') {
        ESP_LOGI(TAG, "RequestAlarmCloudTts via schedule_task_id: %s", schedule_task_id);
        lingxin_trigger_schedule_alarm(schedule_task_id);
        chat_session_active_ = true;
        return;
    }

    const std::string content = message.empty() ? "提醒时间到了" : message;
    alarm_tts_input_buffer_ = content;

    StartNewChatProps start_props = get_start_new_chat_default_props();
    start_props.disable_welcome_audio = true;
    start_props.single_round = true;
    start_props.play_prologue = false;
    start_props.user_input = alarm_tts_input_buffer_.data();
    ApplyStartNewChatProps(start_props);

    ESP_LOGI(TAG, "RequestAlarmCloudTts via user_input: %s", alarm_tts_input_buffer_.c_str());
    const int ret = start_new_chat(&start_props);
    if (ret != 0) {
        ESP_LOGE(TAG, "RequestAlarmCloudTts start_new_chat failed: %d", ret);
        lingxin_set_alarm_alert_turn(0);
        return;
    }
    chat_session_active_ = true;
}

#endif  // CONFIG_LINGXIN_SDK_ENABLE
