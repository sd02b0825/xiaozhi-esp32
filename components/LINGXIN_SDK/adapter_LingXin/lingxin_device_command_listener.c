/**
 * lingxin_device_command_listener.c - Forward system_event payloads to device command handler
 */

#include "lingxin_device_command_listener.h"
#include "lingxin_alarm_trigger.h"
#include "lingxin_protocol_manager.h"
#include "chat_state_machine.h"
#include "chat_state_machine_event.h"
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

void lingxin_trigger_schedule_alarm(const char *schedule_task_id)
{
    if (schedule_task_id == NULL || schedule_task_id[0] == '\0') {
        return;
    }

    char *task_id_copy = lingxin_strdup(schedule_task_id);
    if (task_id_copy == NULL) {
        lingxin_log_error("lingxin_trigger_schedule_alarm: strdup failed");
        return;
    }

    ScheduleTimerPayload schedule_payload = {0};
    schedule_payload.schedule_task_id = task_id_copy;
    schedule_payload.input_mode = "no_voice";
    StateEventPayload payload = {
        .schedule_timer_payload = &schedule_payload,
    };
    state_machine_run_event_with_payload(State_Event_NoVoice_Start, &payload);
    lingxin_free(task_id_copy);
}
