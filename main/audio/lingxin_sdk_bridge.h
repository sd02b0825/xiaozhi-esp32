/**
 * lingxin_sdk_bridge.h - C/C++ bridge declarations
 *
 * Provides extern "C" functions that are called from SDK adapter C code,
 * implemented in lingxin_sdk_bridge.cc to access C++ AudioService/Board.
 */

#ifndef LINGXIN_SDK_BRIDGE_H
#define LINGXIN_SDK_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- AudioService record bridge ---- */

/**
 * Start feeding AudioProcessor PCM output to SDK record ringbuf.
 * Called by lingxin_recorder_open().
 */
void audio_service_start_record_to_sdk(void);

/**
 * Stop feeding AudioProcessor PCM output to SDK record ringbuf.
 * Called by lingxin_recorder_close().
 */
void audio_service_stop_record_to_sdk(void);

/**
 * Write PCM data to SDK record ringbuf.
 * Called from AudioProcessor OnOutput callback when in SDK mode.
 */
int lingxin_record_write_pcm(const uint8_t *data, size_t len);

/**
 * Check if SDK record ringbuf is available.
 */
int lingxin_record_ringbuf_available(void);

/**
 * Check if SDK record mode is active.
 */
int lingxin_sdk_is_record_mode(void);

/**
 * Block until decode/playback queues are empty (local TTS finished).
 */
void audio_service_wait_playback_idle(void);

/**
 * Return 1 if decode or playback queue has pending audio, else 0.
 */
int audio_service_is_playback_busy(void);

/**
 * Return 1 when SDK uplink capture is active (post-playback, during upload).
 */
int audio_service_is_sdk_uplink_active(void);

/**
 * Schedule uplink activation on the main thread: wait playback idle, enable VP,
 * start SDK record mode, then call lingxin_recorder_finish_open().
 */
void audio_service_schedule_recorder_uplink_begin(void *recorder_hdl);

/**
 * Schedule uplink deactivation on the main thread after recorder close.
 */
void audio_service_schedule_recorder_uplink_end(void);

/* ---- AudioService playback bridge ---- */

/**
 * Push encoded audio packet to AudioService decode queue.
 * Called by module_bufferPlay_data().
 * codec: "mp3", "pcm", "opus"
 * sample_rate: typically 16000
 */
void audio_service_push_decode_packet(const uint8_t *data, int len, const char *codec, int sample_rate);

/**
 * Reset AudioService decoder (stop current playback).
 * Called by module_bufferPlay_terminate().
 */
void audio_service_reset_decoder(void);

void audio_service_begin_downlink_playback(void);

/**
 * Flush any partial PCM frame left after downlink stream ends.
 * Called by module_bufferPlay_audioEnd().
 */
void audio_service_flush_playback_pending(void);

/**
 * Called when downlink prebuffer is ready; triggers speaking state transition.
 */
void lingxin_notify_downlink_playback_ready(void);

/**
 * Set the processor task priority (used by SDK uplink).
 */
void audio_service_set_processor_task_priority(int priority);

/* ---- AudioService local sound bridge ---- */

/**
 * Play a local sound file via AudioService.
 * Called by lingxin_local_player_play().
 * audio_path: filesystem path to audio file
 */
void audio_service_play_local_sound(const char *audio_path);

/**
 * Set speaker output volume (0-100) and persist to NVS.
 * Called by LingXin SDK set_volume() adapter path.
 */
void audio_service_set_output_volume(int volume);

/**
 * Record that a volume system instruction was executed locally this turn.
 * Does not suppress TTS until a conflicting agent_response_text is seen.
 */
void lingxin_mark_volume_command_handled(int target_volume);

/** Return 1 if a volume command was handled in the current dialogue turn. */
int lingxin_volume_command_handled_this_turn(void);

/** Enable/disable dropping cloud TTS packets for the rest of this turn. */
void lingxin_set_suppress_cloud_tts(int suppress);

/** Return 1 if cloud downlink TTS should be suppressed for this turn. */
int lingxin_should_suppress_cloud_tts(void);

/** Last target volume from a locally handled command, or -1. */
int lingxin_get_last_volume_command_target(void);

/** Clear suppress flag after downlink stream ends. */
void lingxin_clear_volume_command_suppress(void);

/** Mark that device should enter standby after current downlink TTS finishes. */
void lingxin_mark_standby_after_playback(void);

/** Return 1 if standby was requested for after current playback. */
int lingxin_standby_after_playback_pending(void);

void lingxin_clear_standby_after_playback(void);

/** Block AbortSpeaking/Wakeup_Detected while gracefully exiting to standby. */
void lingxin_set_standby_exit_in_progress(int in_progress);

int lingxin_standby_exit_in_progress(void);

/**
 * 本地闹钟到点后请求云端 TTS 播报。
 * @param message 提醒内容，如「喝水」
 * @param schedule_task_id 云端 schedule_task_id（可选，优先使用）
 */
void lingxin_request_alarm_cloud_tts(const char *message, const char *schedule_task_id);

/** 标记当前为本地闹钟到点播报轮次（此轮内忽略云端误下发的 SET_ALARM） */
void lingxin_set_alarm_alert_turn(int active);

/** 是否处于本地闹钟到点播报轮次 */
int lingxin_is_alarm_alert_turn(void);

/* ---- Device info bridge ---- */

/**
 * Get device name from v2.6.6 Board.
 */
const char *lingxin_bridge_get_device_name(void);

/**
 * Get device version from v2.6.6 build info.
 */
const char *lingxin_bridge_get_device_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LINGXIN_SDK_BRIDGE_H */
