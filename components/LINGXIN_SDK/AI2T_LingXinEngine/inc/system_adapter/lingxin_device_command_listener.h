/**
 * lingxin_device_command_listener.h - Register SDK protocol listener for device commands
 */

#ifndef LINGXIN_DEVICE_COMMAND_LISTENER_H
#define LINGXIN_DEVICE_COMMAND_LISTENER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register WebSocket event listener for system_event payloads that carry
 * volume control instructions from the LingXin cloud.
 */
void lingxin_adapter_init_device_command_listener(void);

/**
 * Schedule volume command handling on the application main thread.
 * Implemented in main/audio/lingxin_device_command.cc.
 */
void lingxin_schedule_device_command_payload(const char* payload);

#ifdef __cplusplus
}
#endif

#endif /* LINGXIN_DEVICE_COMMAND_LISTENER_H */
