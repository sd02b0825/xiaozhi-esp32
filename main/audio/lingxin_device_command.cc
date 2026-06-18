/**
 * lingxin_device_command.cc - LingXin device system instruction handler
 *
 * 灵芯 text_output 实际格式（优先）：
 * {"type":"intent_recognition_result","result":[{"name":"MAX_VOLUME","arguments":{}}]}
 * {"type":"action_list","result":[{"action":"MAX_VOLUME"}]}
 * {"type":"intent_recognition_result","result":[{"name":"STANDBY","arguments":{}}]}
 *
 * 兼容扁平格式：
 * {"command":"INCREASE_VOLUME_BY","params":{"VOLUME":10}}
 * {"func":"CHANGE_VOLUME_TO","props":{"VOLUME":50}}
 */

#include "lingxin_device_command.h"

#include "application.h"
#include "boards/common/board.h"
#include "display.h"
#include "assets/lang_config.h"
#include "lingxin_sdk_bridge.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

static const char* TAG = "LxDeviceCmd";

static int volume_before_mute_ = -1;
static int64_t last_volume_cmd_us_ = 0;
static int last_volume_target_applied_ = -1;
static std::string last_user_asr_text_;

static int ClampVolume(int volume)
{
    return std::max(0, std::min(100, volume));
}

static void ShowVolumeNotification(int volume)
{
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }
}

static void ShowMutedNotification()
{
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(Lang::Strings::MUTED);
    }
}

static void ShowMaxVolumeNotification()
{
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(Lang::Strings::MAX_VOLUME);
    }
}

static std::string ToLowerAscii(const char* text)
{
    if (text == nullptr) {
        return {};
    }
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool IsVolumeCommandName(const char* command)
{
    return command != nullptr &&
           (strcmp(command, "INCREASE_VOLUME_BY") == 0 || strcmp(command, "DECREASE_VOLUME_BY") == 0 ||
            strcmp(command, "CHANGE_VOLUME_TO") == 0 || strcmp(command, "MAX_VOLUME") == 0 ||
            strcmp(command, "MUTE") == 0 || strcmp(command, "UNMUTE") == 0);
}

static bool IsStandbyCommandName(const char* command)
{
    return command != nullptr && strcmp(command, "STANDBY") == 0;
}

static const char* NormalizeVolumeCommand(const char* raw_name)
{
    if (raw_name == nullptr || raw_name[0] == '\0') {
        return nullptr;
    }
    if (IsVolumeCommandName(raw_name)) {
        return raw_name;
    }

    const std::string name = ToLowerAscii(raw_name);
    if (name == "increase_volume_by" || name == "increase_volum_by" || name == "increase_volume" ||
        name == "volume_up") {
        return "INCREASE_VOLUME_BY";
    }
    if (name == "decrease_volume_by" || name == "decrease_volume" || name == "volume_down") {
        return "DECREASE_VOLUME_BY";
    }
    if (name == "change_volume_to" || name == "set_volume" || name == "change_volume") {
        return "CHANGE_VOLUME_TO";
    }
    if (name == "max_volume" || name == "volume_max") {
        return "MAX_VOLUME";
    }
    if (name == "mute" || name == "mute_volume") {
        return "MUTE";
    }
    if (name == "unmute" || name == "unmute_volume") {
        return "UNMUTE";
    }
    return nullptr;
}

static const char* NormalizeStandbyCommand(const char* raw_name)
{
    if (raw_name == nullptr || raw_name[0] == '\0') {
        return nullptr;
    }
    if (IsStandbyCommandName(raw_name)) {
        return raw_name;
    }

    const std::string name = ToLowerAscii(raw_name);
    if (name == "standby" || name == "go_standby" || name == "enter_standby" || name == "close" ||
        name == "exit" || name == "exit_chat" || name == "shutdown_chat") {
        return "STANDBY";
    }
    return nullptr;
}

static const char* NormalizeDeviceCommand(const char* raw_name)
{
    const char* volume_command = NormalizeVolumeCommand(raw_name);
    if (volume_command != nullptr) {
        return volume_command;
    }
    return NormalizeStandbyCommand(raw_name);
}

static std::string TrimStandbyVoiceText(const char* text)
{
    if (text == nullptr) {
        return {};
    }

    std::string trimmed(text);
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    static const char* kTrailingPunctuation = "。！？.!?，,";
    while (!trimmed.empty() && std::strchr(kTrailingPunctuation, trimmed.back()) != nullptr) {
        trimmed.pop_back();
    }
    return trimmed;
}

static bool IsStandbyVoiceText(const char* text)
{
    return TrimStandbyVoiceText(text) == "关闭";
}

static void MaybeStoreUserText(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const std::string trimmed = TrimStandbyVoiceText(text);
    if (!trimmed.empty()) {
        last_user_asr_text_ = trimmed;
    }
}

static void ExtractUserTextFromObject(const cJSON* object)
{
    if (!cJSON_IsObject(object)) {
        return;
    }

    static const char* kTextKeys[] = {"text", "query", "user_input", "utterance", "content", "asr_text", "input"};
    for (const char* key : kTextKeys) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            MaybeStoreUserText(item->valuestring);
        }
    }

    static const char* kNestedKeys[] = {"arguments", "slots", "params", "props"};
    for (const char* key : kNestedKeys) {
        ExtractUserTextFromObject(cJSON_GetObjectItemCaseSensitive(object, key));
    }
}

void lingxin_device_command_note_text_output(const cJSON* root)
{
    if (root == nullptr) {
        return;
    }

    const cJSON* type_field = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* type = cJSON_IsString(type_field) ? type_field->valuestring : nullptr;
    if (type != nullptr &&
        (strstr(type, "asr") != nullptr || strstr(type, "input") != nullptr ||
         strstr(type, "query") != nullptr || strstr(type, "user") != nullptr)) {
        const cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (cJSON_IsString(result)) {
            MaybeStoreUserText(result->valuestring);
        }
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            MaybeStoreUserText(text->valuestring);
        }
    }

    const cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsArray(result)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, result) {
            ExtractUserTextFromObject(item);
        }
    } else if (cJSON_IsObject(result)) {
        ExtractUserTextFromObject(result);
    }

    ExtractUserTextFromObject(root);
}

void lingxin_device_command_clear_turn_state(void)
{
    last_user_asr_text_.clear();
}

bool lingxin_device_command_try_handle_pending_user_text(void)
{
    if (last_user_asr_text_.empty()) {
        return false;
    }
    return lingxin_device_command_try_handle_user_text(last_user_asr_text_.c_str());
}

bool lingxin_device_command_is_mute_reply_for_standby(const char* agent_text)
{
    if (agent_text == nullptr || !IsStandbyVoiceText(last_user_asr_text_.c_str())) {
        return false;
    }
    return strstr(agent_text, "声音") != nullptr && strstr(agent_text, "关") != nullptr;
}

bool lingxin_device_command_is_standby_farewell_agent_text(const char* agent_text)
{
    if (agent_text == nullptr || agent_text[0] == '\0') {
        return false;
    }

    /* 云端正确理解「关闭」后常见的告别话术 */
    if (strstr(agent_text, "随时叫我") != nullptr || strstr(agent_text, "随时找") != nullptr ||
        strstr(agent_text, "下次再聊") != nullptr || strstr(agent_text, "有需要再") != nullptr ||
        strstr(agent_text, "先休息") != nullptr) {
        return true;
    }
    if (strstr(agent_text, "之后") != nullptr && strstr(agent_text, "聊") != nullptr &&
        strstr(agent_text, "随时") != nullptr) {
        return true;
    }
    return false;
}

static int ParseJsonInt(const cJSON* item)
{
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return atoi(item->valuestring);
    }
    return -1;
}

static int ExtractVolumeFromObject(const cJSON* object)
{
    if (!cJSON_IsObject(object)) {
        return -1;
    }

    static const char* kVolumeKeys[] = {"VOLUME", "volume", "number", "amount", "value", "to"};
    for (const char* key : kVolumeKeys) {
        int volume = ParseJsonInt(cJSON_GetObjectItemCaseSensitive(object, key));
        if (volume >= 0) {
            return volume;
        }
    }

    const cJSON* slots = cJSON_GetObjectItemCaseSensitive(object, "slots");
    if (cJSON_IsObject(slots)) {
        return ExtractVolumeFromObject(slots);
    }
    return -1;
}

static int GetVolumeParam(const cJSON* root)
{
    if (root == nullptr) {
        return -1;
    }

    int volume = ExtractVolumeFromObject(root);
    if (volume >= 0) {
        return volume;
    }

    static const char* kNestedKeys[] = {"params", "props", "payload", "arguments"};
    for (const char* key : kNestedKeys) {
        volume = ExtractVolumeFromObject(cJSON_GetObjectItemCaseSensitive(root, key));
        if (volume >= 0) {
            return volume;
        }
    }
    return -1;
}

static const char* GetCommandFromObject(const cJSON* object)
{
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }

    /* name 用于 intent_recognition_result，action 用于 action_list */
    static const char* kCommandKeys[] = {"command", "func", "instruction", "action", "name"};
    for (const char* key : kCommandKeys) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            const char* canonical = NormalizeDeviceCommand(item->valuestring);
            if (canonical != nullptr) {
                return canonical;
            }
        }
    }
    return nullptr;
}

static const char* GetCommandName(const cJSON* root)
{
    if (root == nullptr) {
        return nullptr;
    }

    const char* command = GetCommandFromObject(root);
    if (command != nullptr) {
        return command;
    }

    static const char* kNestedKeys[] = {"payload", "params", "props", "data"};
    for (const char* key : kNestedKeys) {
        command = GetCommandFromObject(cJSON_GetObjectItemCaseSensitive(root, key));
        if (command != nullptr) {
            return command;
        }
    }

    const cJSON* header = cJSON_GetObjectItemCaseSensitive(root, "header");
    if (cJSON_IsObject(header)) {
        const cJSON* action = cJSON_GetObjectItemCaseSensitive(header, "action");
        if (cJSON_IsString(action) && action->valuestring != nullptr) {
            return NormalizeDeviceCommand(action->valuestring);
        }
    }
    return nullptr;
}

bool lingxin_device_command_handle(const char* command, int volume_param)
{
    const char* standby_command = NormalizeStandbyCommand(command);
    if (standby_command != nullptr) {
        ESP_LOGI(TAG, "Standby command %s received", standby_command);
        lingxin_set_standby_exit_in_progress(1);
        Application::GetInstance().EnterStandby();
        return true;
    }

    const char* canonical = NormalizeVolumeCommand(command);
    if (canonical == nullptr) {
        return false;
    }
    command = canonical;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGW(TAG, "No audio codec available for command %s", command);
        return false;
    }

    const int current = codec->output_volume();
    const int step = volume_param > 0 ? volume_param : 10;
    int target = current;

    if (strcmp(command, "INCREASE_VOLUME_BY") == 0) {
        target = ClampVolume(current + step);
    } else if (strcmp(command, "DECREASE_VOLUME_BY") == 0) {
        target = ClampVolume(current - step);
    } else if (strcmp(command, "CHANGE_VOLUME_TO") == 0) {
        target = ClampVolume(volume_param >= 0 ? volume_param : current);
    } else if (strcmp(command, "MAX_VOLUME") == 0) {
        target = 100;
    } else if (strcmp(command, "MUTE") == 0) {
        if (IsStandbyVoiceText(last_user_asr_text_.c_str())) {
            ESP_LOGI(TAG, "Remap cloud MUTE to STANDBY for user text: %s", last_user_asr_text_.c_str());
            lingxin_set_suppress_cloud_tts(1);
            lingxin_set_standby_exit_in_progress(1);
            Application::GetInstance().EnterStandby();
            return true;
        }
        if (current > 0) {
            volume_before_mute_ = current;
        }
        target = 0;
    } else if (strcmp(command, "UNMUTE") == 0) {
        target = volume_before_mute_ > 0 ? volume_before_mute_ : 50;
    }

    const int64_t now_us = esp_timer_get_time();
    if (target == last_volume_target_applied_ && (now_us - last_volume_cmd_us_) < 500 * 1000) {
        ESP_LOGI(TAG, "Volume command %s deduplicated: target still %d", command, target);
        return true;
    }

    codec->SetOutputVolume(target);
    if (strcmp(command, "MAX_VOLUME") == 0) {
        ShowMaxVolumeNotification();
    } else if (strcmp(command, "MUTE") == 0) {
        ShowMutedNotification();
    } else {
        ShowVolumeNotification(target);
    }

    last_volume_target_applied_ = target;
    last_volume_cmd_us_ = now_us;
    lingxin_mark_volume_command_handled(target);

    ESP_LOGI(TAG, "Volume command %s applied: %d -> %d", command, current, target);
    return true;
}

static bool TryHandleCommandObject(const cJSON* command_obj)
{
    const char* command = GetCommandFromObject(command_obj);
    if (command == nullptr) {
        return false;
    }
    return lingxin_device_command_handle(command, GetVolumeParam(command_obj));
}

static bool TryHandleResultArray(const cJSON* result)
{
    if (!cJSON_IsArray(result)) {
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, result) {
        if (TryHandleCommandObject(item)) {
            return true;
        }
    }
    return false;
}

bool lingxin_device_command_try_handle_json(const cJSON* root)
{
    if (root == nullptr) {
        return false;
    }

    /* 灵芯 text_output：intent_recognition_result / action_list */
    if (TryHandleResultArray(cJSON_GetObjectItemCaseSensitive(root, "result"))) {
        return true;
    }

    /* 第一版扁平/嵌套格式 */
    const char* command = GetCommandName(root);
    if (command != nullptr) {
        return lingxin_device_command_handle(command, GetVolumeParam(root));
    }

    return false;
}

bool lingxin_device_command_try_handle_payload(const char* payload)
{
    if (payload == nullptr || payload[0] == '\0') {
        return false;
    }

    cJSON* root = cJSON_Parse(payload);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid command payload JSON: %.128s", payload);
        return false;
    }

    bool handled = lingxin_device_command_try_handle_json(root);
    if (!handled) {
        ESP_LOGI(TAG, "Unrecognized command payload: %.300s", payload);
    }

    cJSON_Delete(root);
    return handled;
}

void lingxin_schedule_device_command_payload(const char* payload)
{
    if (payload == nullptr || payload[0] == '\0') {
        return;
    }

    std::string payload_copy(payload);
    Application::GetInstance().Schedule([payload_copy]() {
        lingxin_device_command_try_handle_payload(payload_copy.c_str());
    });
}

bool lingxin_device_command_try_handle_user_text(const char* text)
{
    if (!IsStandbyVoiceText(text)) {
        return false;
    }

    ESP_LOGI(TAG, "Standby voice text detected: %s", text);
    lingxin_set_suppress_cloud_tts(1);
    lingxin_set_standby_exit_in_progress(1);
    Application::GetInstance().EnterStandby();
    return true;
}
