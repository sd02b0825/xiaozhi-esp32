/**
 * lingxin_device_command.cc - LingXin device system instruction handler
 *
 * 灵芯 text_output 实际格式（优先）：
 * {"type":"intent_recognition_result","result":[{"name":"MAX_VOLUME","arguments":{}}]}
 * {"type":"action_list","result":[{"action":"MAX_VOLUME"}]}
 * {"type":"intent_recognition_result","result":[{"name":"STANDBY","arguments":{}}]}
 * {"type":"action_list","result":[{"action":"SET_ALARM","params":{"CONTENT":"吃饭","TRIGGER_TIME":"2026-06-18 11:40:00 Thursday","REPEAT":"WEEKLY:1,2,3,4,5"}}]}
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
#include <ctime>
#include <cstring>
#include <string>
#include <strings.h>

static const char* TAG = "LxDeviceCmd";

/** 本地闹钟：云端 SET_ALARM 可同时保留多个，参数 TRIGGER_TIME / CONTENT / REPEAT */
static constexpr size_t kMaxLocalAlarms = 20;

enum class AlarmRepeatType { None, Daily, Weekly };

struct AlarmRepeatRule {
    AlarmRepeatType type = AlarmRepeatType::None;
    uint8_t weekday_mask = 0; /* bit N = tm_wday N（0=周日） */
};

struct LocalAlarmSlot {
    bool active = false;
    uint32_t id = 0;
    esp_timer_handle_t timer = nullptr;
    std::string content;
    std::string repeat_raw;
    AlarmRepeatRule repeat;
    int trigger_hour = -1;
    int trigger_minute = -1;
    int trigger_second = 0;
    time_t next_trigger_ts = 0;
    std::string schedule_task_id;
    std::string dedup_key;
};

static LocalAlarmSlot alarm_slots_[kMaxLocalAlarms];
static uint32_t next_alarm_id_ = 1;

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

static bool IsSetAlarmCommandName(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    return strcasecmp(name, "SET_ALARM") == 0;
}

static const char* GetRawCommandName(const cJSON* object)
{
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }

    static const char* kCommandKeys[] = {"command", "func", "instruction", "action", "name"};
    for (const char* key : kCommandKeys) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
        if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
            return item->valuestring;
        }
    }
    return nullptr;
}

static const char* GetStringField(const cJSON* object, const char* key)
{
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr && item->valuestring[0] != '\0') {
        return item->valuestring;
    }
    return nullptr;
}

static void ExtractAlarmFields(const cJSON* object, std::string* content, std::string* trigger_time,
                               std::string* repeat, std::string* schedule_task_id)
{
    if (!cJSON_IsObject(object)) {
        return;
    }

    static const char* kNestedKeys[] = {"params", "arguments", "props", "payload"};
    for (const char* nested_key : kNestedKeys) {
        const cJSON* nested = cJSON_GetObjectItemCaseSensitive(object, nested_key);
        if (!cJSON_IsObject(nested)) {
            continue;
        }

        const char* content_value = GetStringField(nested, "CONTENT");
        if (content_value == nullptr) {
            content_value = GetStringField(nested, "content");
        }
        if (content_value != nullptr) {
            *content = content_value;
        }

        const char* trigger_value = GetStringField(nested, "TRIGGER_TIME");
        if (trigger_value == nullptr) {
            trigger_value = GetStringField(nested, "trigger_time");
        }
        if (trigger_value != nullptr) {
            *trigger_time = trigger_value;
        }

        const char* repeat_value = GetStringField(nested, "REPEAT");
        if (repeat_value == nullptr) {
            repeat_value = GetStringField(nested, "repeat");
        }
        if (repeat_value != nullptr) {
            *repeat = repeat_value;
        }

        const char* task_id_value = GetStringField(nested, "SCHEDULE_TASK_ID");
        if (task_id_value == nullptr) {
            task_id_value = GetStringField(nested, "schedule_task_id");
        }
        if (task_id_value != nullptr) {
            *schedule_task_id = task_id_value;
        }
    }
}

static int IsoWeekdayToTmWday(int iso_day)
{
    if (iso_day >= 1 && iso_day <= 6) {
        return iso_day;
    }
    if (iso_day == 7) {
        return 0;
    }
    return -1;
}

static void SetWeekdayMaskBit(AlarmRepeatRule* rule, int tm_wday)
{
    if (rule == nullptr || tm_wday < 0 || tm_wday > 6) {
        return;
    }
    rule->weekday_mask |= static_cast<uint8_t>(1u << tm_wday);
}

static bool ParseWeekdayToken(const std::string& token, AlarmRepeatRule* rule)
{
    if (token.empty() || rule == nullptr) {
        return false;
    }

    static const struct {
        const char* name;
        int tm_wday;
    } kWeekdayNames[] = {
        {"sun", 0}, {"sunday", 0}, {"mon", 1}, {"monday", 1}, {"tue", 2}, {"tuesday", 2},
        {"wed", 3}, {"wednesday", 3}, {"thu", 4}, {"thursday", 4}, {"fri", 5}, {"friday", 5},
        {"sat", 6}, {"saturday", 6},
    };

    for (const auto& entry : kWeekdayNames) {
        if (token == entry.name) {
            SetWeekdayMaskBit(rule, entry.tm_wday);
            return true;
        }
    }

    static const struct {
        const char* name;
        int tm_wday;
    } kChineseWeekdays[] = {
        {"周日", 0}, {"周天", 0}, {"星期一", 1}, {"周一", 1}, {"星期二", 2}, {"周二", 2},
        {"星期三", 3}, {"周三", 3}, {"星期四", 4}, {"周四", 4}, {"星期五", 5}, {"周五", 5},
        {"星期六", 6}, {"周六", 6},
    };
    for (const auto& entry : kChineseWeekdays) {
        if (token.find(entry.name) != std::string::npos) {
            SetWeekdayMaskBit(rule, entry.tm_wday);
            return true;
        }
    }

    char* end = nullptr;
    const long iso_day = strtol(token.c_str(), &end, 10);
    if (end != token.c_str() && *end == '\0') {
        const int tm_wday = IsoWeekdayToTmWday(static_cast<int>(iso_day));
        if (tm_wday >= 0) {
            SetWeekdayMaskBit(rule, tm_wday);
            return true;
        }
    }
    return false;
}

/** 解析 REPEAT：空=单次；DAILY/每天；WEEKLY:1,2,3,4,5 / MON,TUE / 周一,周二 */
static bool ParseRepeatRule(const char* raw, AlarmRepeatRule* rule)
{
    if (rule == nullptr) {
        return false;
    }
    rule->type = AlarmRepeatType::None;
    rule->weekday_mask = 0;
    if (raw == nullptr || raw[0] == '\0') {
        return true;
    }

    std::string normalized = ToLowerAscii(raw);
    if (normalized.find("daily") != std::string::npos || normalized.find("everyday") != std::string::npos ||
        normalized.find("每天") != std::string::npos) {
        rule->type = AlarmRepeatType::Daily;
        return true;
    }

    std::string payload = normalized;
    bool has_weekly_keyword = normalized == "weekly" || normalized == "week" || normalized.find("weekly:") == 0 ||
                              normalized.find("week:") == 0 || normalized.find("每周") != std::string::npos;
    for (const char* prefix : {"weekly:", "week:"}) {
        const size_t pos = payload.find(prefix);
        if (pos != std::string::npos) {
            payload = payload.substr(pos + strlen(prefix));
            has_weekly_keyword = true;
            break;
        }
    }

    AlarmRepeatRule token_rule = {};
    if (ParseWeekdayToken(normalized, &token_rule) && token_rule.weekday_mask != 0) {
        rule->type = AlarmRepeatType::Weekly;
        rule->weekday_mask = token_rule.weekday_mask;
    } else if (has_weekly_keyword || payload.find(',') != std::string::npos) {
        rule->type = AlarmRepeatType::Weekly;
    }

    if (rule->type != AlarmRepeatType::Weekly) {
        return true;
    }

    size_t start = 0;
    while (start <= payload.size()) {
        const size_t comma = payload.find(',', start);
        const std::string token = payload.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        ParseWeekdayToken(token, rule);
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    if (rule->weekday_mask == 0) {
        ParseWeekdayToken(normalized, rule);
    }
    return has_weekly_keyword || rule->weekday_mask != 0 || payload.find(',') != std::string::npos;
}

static std::string TrimAscii(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

static bool IsImmediateTriggerTime(const char* raw)
{
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const std::string normalized = TrimAscii(ToLowerAscii(raw));
    return normalized == "now" || normalized == "immediate" || normalized == "asap" ||
           normalized == "立刻" || normalized == "马上" || normalized == "现在";
}

static bool ParseTriggerTime(const char* raw, time_t* trigger_ts, struct tm* tm_out)
{
    if (raw == nullptr || trigger_ts == nullptr) {
        return false;
    }

    if (IsImmediateTriggerTime(raw)) {
        const time_t now_ts = time(nullptr);
        if (now_ts < 1600000000) {
            return false;
        }
        *trigger_ts = now_ts + 3;
        if (tm_out != nullptr) {
            localtime_r(trigger_ts, tm_out);
        }
        return true;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(raw, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    struct tm tm_value = {};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    tm_value.tm_isdst = -1;

    *trigger_ts = mktime(&tm_value);
    if (*trigger_ts == static_cast<time_t>(-1)) {
        return false;
    }
    if (tm_out != nullptr) {
        localtime_r(trigger_ts, tm_out);
    }
    return true;
}

static void FormatLocalTime(time_t ts, char* buf, size_t len)
{
    struct tm tm_value;
    localtime_r(&ts, &tm_value);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_value);
}

/** 将任意 int 时刻钳制为 HH:MM，避免 snprintf 因 int 全范围触发 -Wformat-truncation */
static void FormatAlarmClock(int hour, int minute, char* buf, size_t buf_len)
{
    const unsigned safe_hour = static_cast<unsigned>(((hour % 24) + 24) % 24);
    const unsigned safe_minute = static_cast<unsigned>(((minute % 60) + 60) % 60);
    snprintf(buf, buf_len, "%02u:%02u", safe_hour, safe_minute);
}

/** 修正云端 TRIGGER_TIME 日期偏差（常见：用户要今天，云端填成明天） */
static time_t NormalizeTriggerTimestamp(time_t cloud_ts, time_t now_ts, const struct tm& cloud_tm)
{
    int64_t delay_sec = static_cast<int64_t>(cloud_ts) - static_cast<int64_t>(now_ts);

    if (delay_sec > 6 * 3600) {
        struct tm now_tm;
        localtime_r(&now_ts, &now_tm);
        struct tm today_tm = cloud_tm;
        today_tm.tm_year = now_tm.tm_year;
        today_tm.tm_mon = now_tm.tm_mon;
        today_tm.tm_mday = now_tm.tm_mday;
        today_tm.tm_isdst = -1;
        const time_t today_ts = mktime(&today_tm);
        const int64_t today_delay = static_cast<int64_t>(today_ts) - static_cast<int64_t>(now_ts);
        if (today_delay > 0 && today_delay < delay_sec) {
            ESP_LOGW(TAG, "SET_ALARM: cloud date too far, use today %02d:%02d (delay %ld -> %ld sec)",
                     cloud_tm.tm_hour, cloud_tm.tm_min, static_cast<long>(delay_sec),
                     static_cast<long>(today_delay));
            return today_ts;
        }
    }

    if (delay_sec <= 0 && delay_sec >= -300) {
        ESP_LOGW(TAG, "SET_ALARM: trigger just passed, fire in 3 sec");
        return now_ts + 3;
    }

    return cloud_ts;
}

static size_t CountActiveAlarms();

static void ShowAlarmScheduledNotification(int hour, int minute)
{
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    char buf[48];
    char time_buf[8];
    FormatAlarmClock(hour, minute, time_buf, sizeof(time_buf));
    snprintf(buf, sizeof(buf), "已设置提醒 %s（共%u个）", time_buf, static_cast<unsigned>(CountActiveAlarms()));
    display->ShowNotification(buf);
}

/** 生成中文口语时间，如「下午2点10分」 */
static std::string FormatAlarmTimeSpeechLabel(int hour, int minute)
{
    const char* period = "上午";
    int display_hour = hour;

    if (hour >= 0 && hour < 6) {
        period = "凌晨";
    } else if (hour < 12) {
        period = "上午";
    } else if (hour == 12) {
        period = "中午";
    } else if (hour < 18) {
        period = "下午";
        display_hour = hour - 12;
    } else {
        period = "晚上";
        display_hour = hour - 12;
    }

    char buf[32];
    if (minute == 0) {
        snprintf(buf, sizeof(buf), "%s%d点整", period, display_hour);
    } else {
        snprintf(buf, sizeof(buf), "%s%d点%d分", period, display_hour, minute);
    }
    return buf;
}

static std::string BuildAlarmDisplayMessage(int hour, int minute, const std::string& content)
{
    if (hour < 0 || minute < 0) {
        return content.empty() ? "提醒" : content;
    }
    char time_buf[8];
    FormatAlarmClock(hour, minute, time_buf, sizeof(time_buf));
    if (content.empty()) {
        return time_buf;
    }
    return std::string(time_buf) + " " + content;
}

/** 发给云端的 user_input：明确是已到点播报，避免云端当成「设置提醒」 */
static std::string BuildAlarmTtsPrompt(int hour, int minute, const std::string& content)
{
    const std::string topic = content.empty() ? "您设置的提醒" : content;
    if (hour < 0 || minute < 0) {
        return "【闹钟到点】请直接口头提醒用户：" + topic + "。不要设置新闹钟。";
    }
    return "【闹钟到点】现在是" + FormatAlarmTimeSpeechLabel(hour, minute) +
           "，请直接口头提醒用户：" + topic + "。不要设置新闹钟。";
}

static time_t BuildTimestampFromLocalTime(int hour, int minute, int second, const struct tm& day_tm)
{
    struct tm tm_value = day_tm;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    tm_value.tm_isdst = -1;
    return mktime(&tm_value);
}

static time_t ComputeNextDailyTrigger(int hour, int minute, int second, time_t after_ts)
{
    struct tm day_tm;
    localtime_r(&after_ts, &day_tm);
    time_t candidate = BuildTimestampFromLocalTime(hour, minute, second, day_tm);
    if (candidate <= after_ts) {
        candidate += 24 * 3600;
    }
    return candidate;
}

static time_t ComputeNextWeeklyTrigger(int hour, int minute, int second, uint8_t weekday_mask, time_t after_ts)
{
    if (weekday_mask == 0) {
        return 0;
    }

    for (int day_offset = 0; day_offset <= 8; ++day_offset) {
        const time_t day_ts = after_ts + static_cast<time_t>(day_offset) * 24 * 3600;
        struct tm day_tm;
        localtime_r(&day_ts, &day_tm);
        if ((weekday_mask & (1u << day_tm.tm_wday)) == 0) {
            continue;
        }
        const time_t candidate = BuildTimestampFromLocalTime(hour, minute, second, day_tm);
        if (candidate > after_ts) {
            return candidate;
        }
    }
    return 0;
}

static time_t ComputeInitialTriggerTs(const struct tm& trigger_tm, time_t parsed_ts, time_t now_ts,
                                      const AlarmRepeatRule& repeat)
{
    if (repeat.type == AlarmRepeatType::None) {
        return NormalizeTriggerTimestamp(parsed_ts, now_ts, trigger_tm);
    }

    const int hour = trigger_tm.tm_hour;
    const int minute = trigger_tm.tm_min;
    const int second = trigger_tm.tm_sec;

    if (repeat.type == AlarmRepeatType::Daily) {
        time_t next = ComputeNextDailyTrigger(hour, minute, second, now_ts);
        if (parsed_ts > now_ts) {
            struct tm parsed_local;
            localtime_r(&parsed_ts, &parsed_local);
            if (parsed_local.tm_hour == hour && parsed_local.tm_min == minute &&
                parsed_local.tm_sec == second && parsed_ts < next) {
                return parsed_ts;
            }
        }
        return next;
    }

    AlarmRepeatRule weekly_rule = repeat;
    if (weekly_rule.weekday_mask == 0) {
        SetWeekdayMaskBit(&weekly_rule, trigger_tm.tm_wday);
    }
    if (parsed_ts > now_ts && (weekly_rule.weekday_mask & (1u << trigger_tm.tm_wday))) {
        return parsed_ts;
    }
    return ComputeNextWeeklyTrigger(hour, minute, second, weekly_rule.weekday_mask, now_ts);
}

static std::string BuildAlarmDedupKey(const std::string& content, int hour, int minute, time_t trigger_ts,
                                      const AlarmRepeatRule& repeat, const std::string& repeat_raw,
                                      const std::string& schedule_task_id, const char* trigger_time_raw)
{
    if (!schedule_task_id.empty()) {
        return "sid:" + schedule_task_id;
    }

    char time_buf[8];
    FormatAlarmClock(hour, minute, time_buf, sizeof(time_buf));
    std::string key = std::string(time_buf) + "|" + content;
    if (repeat.type == AlarmRepeatType::None) {
        if (IsImmediateTriggerTime(trigger_time_raw)) {
            key += "|now";
        } else {
            char ts_buf[32];
            snprintf(ts_buf, sizeof(ts_buf), "|%ld", static_cast<long>(trigger_ts));
            key += ts_buf;
        }
    } else if (repeat.type == AlarmRepeatType::Daily) {
        key += "|daily|" + repeat_raw;
    } else if (repeat.type == AlarmRepeatType::Weekly) {
        char mask_buf[32];
        snprintf(mask_buf, sizeof(mask_buf), "|w:%u|%s", repeat.weekday_mask, repeat_raw.c_str());
        key += mask_buf;
    }
    return key;
}

static size_t CountActiveAlarms()
{
    size_t count = 0;
    for (size_t i = 0; i < kMaxLocalAlarms; ++i) {
        if (alarm_slots_[i].active) {
            ++count;
        }
    }
    return count;
}

static int FindAlarmSlotByDedupKey(const std::string& dedup_key)
{
    for (size_t i = 0; i < kMaxLocalAlarms; ++i) {
        if (alarm_slots_[i].active && alarm_slots_[i].dedup_key == dedup_key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int FindFreeAlarmSlot()
{
    for (size_t i = 0; i < kMaxLocalAlarms; ++i) {
        if (!alarm_slots_[i].active) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void StopAndClearAlarmSlot(size_t idx)
{
    if (idx >= kMaxLocalAlarms) {
        return;
    }
    LocalAlarmSlot& slot = alarm_slots_[idx];
    if (slot.timer != nullptr) {
        esp_timer_stop(slot.timer);
        esp_timer_delete(slot.timer);
        slot.timer = nullptr;
    }
    slot = LocalAlarmSlot{};
}

static void RescheduleOrRemoveAlarm(size_t idx);
static void AlarmTimerCallback(void* arg);

static bool EnsureAlarmTimerCreated(size_t idx)
{
    if (idx >= kMaxLocalAlarms) {
        return false;
    }
    LocalAlarmSlot& slot = alarm_slots_[idx];
    if (slot.timer != nullptr) {
        return true;
    }

    esp_timer_create_args_t timer_args = {
        .callback = AlarmTimerCallback,
        .arg = reinterpret_cast<void*>(idx),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lx_alarm",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &slot.timer) == ESP_OK;
}

static bool StartAlarmTimer(size_t idx, time_t trigger_ts)
{
    if (idx >= kMaxLocalAlarms || !alarm_slots_[idx].active) {
        return false;
    }

    const time_t now_ts = time(nullptr);
    int64_t delay_sec = static_cast<int64_t>(trigger_ts) - static_cast<int64_t>(now_ts);
    if (delay_sec <= 0) {
        return false;
    }
    if (!EnsureAlarmTimerCreated(idx)) {
        return false;
    }

    LocalAlarmSlot& slot = alarm_slots_[idx];
    slot.next_trigger_ts = trigger_ts;
    esp_timer_stop(slot.timer);
    const esp_err_t ret = esp_timer_start_once(slot.timer, static_cast<uint64_t>(delay_sec) * 1000000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SET_ALARM[%u]: esp_timer_start_once failed: %s", slot.id, esp_err_to_name(ret));
        return false;
    }
    return true;
}

static void FireAlarmSlot(size_t idx)
{
    if (idx >= kMaxLocalAlarms || !alarm_slots_[idx].active) {
        return;
    }

    const LocalAlarmSlot slot_copy = alarm_slots_[idx];
    const std::string message = slot_copy.content.empty() ? "提醒时间到了" : slot_copy.content;
    const std::string tts_prompt = BuildAlarmTtsPrompt(slot_copy.trigger_hour, slot_copy.trigger_minute, message);
    const std::string display_message =
        (slot_copy.trigger_hour >= 0 && slot_copy.trigger_minute >= 0)
            ? BuildAlarmDisplayMessage(slot_copy.trigger_hour, slot_copy.trigger_minute, message)
            : message;

    ESP_LOGI(TAG, "SET_ALARM[%u] timer fired: %s", slot_copy.id, slot_copy.content.c_str());
    Application::GetInstance().Schedule([display_message, tts_prompt, idx, slot_id = slot_copy.id]() {
        ESP_LOGI(TAG, "SET_ALARM[%u] alerting: %s, tts=%s", slot_id, display_message.c_str(), tts_prompt.c_str());
        Application::GetInstance().Alert("提醒", display_message.c_str(), "happy", "");
        lingxin_request_alarm_cloud_tts(tts_prompt.c_str(), nullptr);
        RescheduleOrRemoveAlarm(idx);
    });
}

static void RescheduleOrRemoveAlarm(size_t idx)
{
    if (idx >= kMaxLocalAlarms || !alarm_slots_[idx].active) {
        return;
    }

    LocalAlarmSlot& slot = alarm_slots_[idx];
    if (slot.repeat.type == AlarmRepeatType::None) {
        ESP_LOGI(TAG, "SET_ALARM[%u] one-shot finished, removing", slot.id);
        StopAndClearAlarmSlot(idx);
        return;
    }

    const time_t now_ts = time(nullptr);
    time_t next_ts = 0;
    if (slot.repeat.type == AlarmRepeatType::Daily) {
        next_ts = ComputeNextDailyTrigger(slot.trigger_hour, slot.trigger_minute, slot.trigger_second, now_ts);
    } else if (slot.repeat.type == AlarmRepeatType::Weekly) {
        AlarmRepeatRule weekly_rule = slot.repeat;
        if (weekly_rule.weekday_mask == 0) {
            struct tm now_tm;
            localtime_r(&now_ts, &now_tm);
            SetWeekdayMaskBit(&weekly_rule, now_tm.tm_wday);
        }
        next_ts = ComputeNextWeeklyTrigger(slot.trigger_hour, slot.trigger_minute, slot.trigger_second,
                                           weekly_rule.weekday_mask, now_ts);
    }

    if (next_ts <= now_ts || !StartAlarmTimer(idx, next_ts)) {
        ESP_LOGW(TAG, "SET_ALARM[%u] failed to reschedule repeat alarm", slot.id);
        StopAndClearAlarmSlot(idx);
        return;
    }

    char trigger_str[32];
    FormatLocalTime(next_ts, trigger_str, sizeof(trigger_str));
    ESP_LOGI(TAG, "SET_ALARM[%u] repeat rescheduled: next=%s", slot.id, trigger_str);
}

static void AlarmTimerCallback(void* arg)
{
    const size_t idx = reinterpret_cast<size_t>(arg);
    FireAlarmSlot(idx);
}

static bool ScheduleLocalAlarm(const char* content, const char* trigger_time_raw, const char* repeat_raw,
                               const char* schedule_task_id)
{
    time_t parsed_ts = 0;
    struct tm trigger_tm = {};
    if (!ParseTriggerTime(trigger_time_raw, &parsed_ts, &trigger_tm)) {
        ESP_LOGW(TAG, "SET_ALARM: invalid TRIGGER_TIME: %s", trigger_time_raw ? trigger_time_raw : "null");
        return false;
    }

    const time_t now_ts = time(nullptr);
    if (now_ts < 1600000000) {
        ESP_LOGW(TAG, "SET_ALARM: system time not synced, cannot schedule alarm");
        return false;
    }

    AlarmRepeatRule repeat = {};
    if (!ParseRepeatRule(repeat_raw, &repeat)) {
        ESP_LOGW(TAG, "SET_ALARM: invalid REPEAT: %s", repeat_raw ? repeat_raw : "null");
        return false;
    }
    if (repeat.type == AlarmRepeatType::Weekly && repeat.weekday_mask == 0) {
        SetWeekdayMaskBit(&repeat, trigger_tm.tm_wday);
    }

    const time_t trigger_ts = ComputeInitialTriggerTs(trigger_tm, parsed_ts, now_ts, repeat);
    localtime_r(&trigger_ts, &trigger_tm);

    int64_t delay_sec = static_cast<int64_t>(trigger_ts) - static_cast<int64_t>(now_ts);
    if (delay_sec <= 0) {
        char now_str[32];
        char trigger_str[32];
        FormatLocalTime(now_ts, now_str, sizeof(now_str));
        FormatLocalTime(trigger_ts, trigger_str, sizeof(trigger_str));
        ESP_LOGW(TAG, "SET_ALARM: trigger time already passed, now=%s trigger=%s raw=%s",
                 now_str, trigger_str, trigger_time_raw);
        return false;
    }

    const std::string content_str = (content != nullptr && content[0] != '\0') ? content : "提醒";
    const std::string repeat_str = (repeat_raw != nullptr) ? repeat_raw : "";
    const std::string schedule_id_str =
        (schedule_task_id != nullptr && schedule_task_id[0] != '\0') ? schedule_task_id : "";
    const std::string dedup_key =
        BuildAlarmDedupKey(content_str, trigger_tm.tm_hour, trigger_tm.tm_min, trigger_ts, repeat, repeat_str,
                           schedule_id_str, trigger_time_raw);

    int slot_idx = FindAlarmSlotByDedupKey(dedup_key);
    if (slot_idx < 0) {
        slot_idx = FindFreeAlarmSlot();
        if (slot_idx < 0) {
            ESP_LOGW(TAG, "SET_ALARM: alarm slots full (%u/%u)", static_cast<unsigned>(CountActiveAlarms()),
                     static_cast<unsigned>(kMaxLocalAlarms));
            return false;
        }
        alarm_slots_[slot_idx].active = true;
        alarm_slots_[slot_idx].id = next_alarm_id_++;
    } else {
        ESP_LOGI(TAG, "SET_ALARM[%u] updating existing alarm slot", alarm_slots_[slot_idx].id);
        if (alarm_slots_[slot_idx].timer != nullptr) {
            esp_timer_stop(alarm_slots_[slot_idx].timer);
        }
    }

    LocalAlarmSlot& slot = alarm_slots_[slot_idx];
    slot.content = content_str;
    slot.repeat_raw = repeat_str;
    slot.repeat = repeat;
    slot.trigger_hour = trigger_tm.tm_hour;
    slot.trigger_minute = trigger_tm.tm_min;
    slot.trigger_second = trigger_tm.tm_sec;
    slot.schedule_task_id = schedule_id_str;
    slot.dedup_key = dedup_key;

    if (!StartAlarmTimer(static_cast<size_t>(slot_idx), trigger_ts)) {
        StopAndClearAlarmSlot(static_cast<size_t>(slot_idx));
        return false;
    }

    char now_str[32];
    char trigger_str[32];
    FormatLocalTime(now_ts, now_str, sizeof(now_str));
    FormatLocalTime(trigger_ts, trigger_str, sizeof(trigger_str));
    ESP_LOGI(TAG,
             "SET_ALARM[%u] scheduled: content=%s, repeat=%s, now=%s, trigger=%s, raw=%s, delay=%ld sec, active=%u",
             slot.id, slot.content.c_str(), slot.repeat_raw.empty() ? "(none)" : slot.repeat_raw.c_str(), now_str,
             trigger_str, trigger_time_raw, static_cast<long>(delay_sec),
             static_cast<unsigned>(CountActiveAlarms()));
    ShowAlarmScheduledNotification(slot.trigger_hour, slot.trigger_minute);
    return true;
}

static bool TryHandleAlarmCommandObject(const cJSON* command_obj)
{
    const char* raw_name = GetRawCommandName(command_obj);
    if (!IsSetAlarmCommandName(raw_name)) {
        return false;
    }

    /* 本地闹钟到点播报轮次内，云端可能误下发 SET_ALARM，忽略以免重复设提醒 */
    if (lingxin_is_alarm_alert_turn()) {
        ESP_LOGI(TAG, "SET_ALARM ignored during local alarm alert turn");
        return true;
    }

    std::string content;
    std::string trigger_time;
    std::string repeat;
    std::string schedule_task_id;
    ExtractAlarmFields(command_obj, &content, &trigger_time, &repeat, &schedule_task_id);
    if (trigger_time.empty()) {
        ESP_LOGW(TAG, "SET_ALARM missing TRIGGER_TIME");
        return false;
    }

    ESP_LOGI(TAG, "SET_ALARM received: content=%s trigger=%s repeat=%s schedule_task_id=%s",
             content.empty() ? "(none)" : content.c_str(), trigger_time.c_str(),
             repeat.empty() ? "(none)" : repeat.c_str(),
             schedule_task_id.empty() ? "(none)" : schedule_task_id.c_str());
    return ScheduleLocalAlarm(content.c_str(), trigger_time.c_str(),
                              repeat.empty() ? nullptr : repeat.c_str(),
                              schedule_task_id.empty() ? nullptr : schedule_task_id.c_str());
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

static bool TryHandleVolumeStandbyCommandObject(const cJSON* command_obj)
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

    bool any_handled = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, result) {
        if (TryHandleAlarmCommandObject(item)) {
            any_handled = true;
            continue;
        }
        if (TryHandleVolumeStandbyCommandObject(item)) {
            any_handled = true;
        }
    }
    return any_handled;
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
