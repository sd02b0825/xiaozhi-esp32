/**
 * lingxin_device_command_listener.c - Forward system_event payloads to device command handler
 */

#include "lingxin_device_command_listener.h"
#include "lingxin_protocol_manager.h"
#include "lingxin_log.h"
#include "lingxin_memory.h"
#include <stdbool.h>
#include <string.h>

extern void lingxin_schedule_device_command_payload(const char *payload);

static bool s_listener_registered = false;

static void on_protocol_event(ChatEventType event, const char *data, const size_t len)
{
    if (event != CHAT_EVENT_ON_SYSTEM_EVENT || data == NULL || len == 0) {
        return;
    }

    /* system_event payload may contain extra_info.commands JSON string */
    char *payload = (char *)lingxin_malloc(len + 1);
    if (payload == NULL) {
        lingxin_log_error("Device command listener: out of memory");
        return;
    }
    memcpy(payload, data, len);
    payload[len] = '\0';

    lingxin_log_ut_with_args(LINGXIN_DEBUG, "device_command_system_event", "payload: %.256s", payload);

    lingxin_schedule_device_command_payload(payload);
    lingxin_free(payload);
}

void lingxin_adapter_init_device_command_listener(void)
{
    if (s_listener_registered) {
        return;
    }

    if (add_protocol_event_listener(on_protocol_event)) {
        s_listener_registered = true;
        lingxin_log_debug("Device command listener registered");
    } else {
        lingxin_log_error("Failed to register device command listener");
    }
}
