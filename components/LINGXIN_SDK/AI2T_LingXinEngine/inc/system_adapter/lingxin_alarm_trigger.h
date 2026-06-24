#ifndef LINGXIN_ALARM_TRIGGER_H
#define LINGXIN_ALARM_TRIGGER_H

#ifdef __cplusplus
extern "C" {
#endif

/** 用云端 schedule_task_id 触发 no_voice 定时提醒（云端 TTS） */
void lingxin_trigger_schedule_alarm(const char *schedule_task_id);

#ifdef __cplusplus
}
#endif

#endif /* LINGXIN_ALARM_TRIGGER_H */
