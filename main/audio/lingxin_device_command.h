/**
 * lingxin_device_command.h - LingXin cloud system instruction handler
 *
 * Parses volume control commands (INCREASE_VOLUME_BY, MUTE, etc.) and standby
 * commands (STANDBY) from WebSocket JSON payloads and applies them locally.
 */

#ifndef LINGXIN_DEVICE_COMMAND_H
#define LINGXIN_DEVICE_COMMAND_H

#include <stdbool.h>
#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Try to handle a parsed JSON object as a volume system instruction.
 * @return true if the JSON was recognized and handled.
 */
bool lingxin_device_command_try_handle_json(const cJSON* root);

/**
 * Parse a JSON string and try to handle it as a volume system instruction.
 * @return true if recognized and handled.
 */
bool lingxin_device_command_try_handle_payload(const char* payload);

/**
 * Execute a device command by name.
 * @param command  e.g. INCREASE_VOLUME_BY, MUTE, STANDBY
 * @param volume_param  step or target volume; use -1 if not specified
 * @return true if command was recognized
 */
bool lingxin_device_command_handle(const char* command, int volume_param);

/**
 * Try to handle recognized user speech text (e.g. "关闭").
 * @return true if the text triggered a local device action.
 */
bool lingxin_device_command_try_handle_user_text(const char* text);

/**
 * Extract and cache user ASR/query text from a LingXin text_output payload.
 */
void lingxin_device_command_note_text_output(const cJSON* root);

/**
 * Try to handle cached user text as a local standby command.
 * @return true if standby was triggered.
 */
bool lingxin_device_command_try_handle_pending_user_text(void);

/** Clear cached user text at the start of a new dialogue turn. */
void lingxin_device_command_clear_turn_state(void);

/**
 * Return true if cloud mute reply should be treated as standby for cached user text.
 */
bool lingxin_device_command_is_mute_reply_for_standby(const char* agent_text);

/**
 * Return true if cloud agent text is a conversation-ending farewell after 关闭.
 */
bool lingxin_device_command_is_standby_farewell_agent_text(const char* agent_text);

/**
 * Schedule volume command handling on the application main thread.
 */
void lingxin_schedule_device_command_payload(const char* payload);

#ifdef __cplusplus
}
#endif

#endif /* LINGXIN_DEVICE_COMMAND_H */
