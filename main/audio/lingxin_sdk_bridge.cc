/**
 * lingxin_sdk_bridge.cc - C/C++ bridge implementation
 *
 * Implements extern "C" functions declared in lingxin_sdk_bridge.h,
 * providing access from SDK adapter C code to v2.6.6 AudioService/Board C++ objects.
 */

#include "lingxin_sdk_bridge.h"
#include "sdkconfig.h"
#if CONFIG_LINGXIN_SDK_ENABLE
#include "protocols/lingxin_sdk_protocol.h"
#endif
#include "audio_service.h"
#include "application.h"
#include "device_state.h"
#include "boards/common/board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string>
#include <atomic>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#endif

/* Fallback if Kconfig is not configured */
#ifndef CONFIG_LINGXIN_AUDIO_DOWN_CODEC
#define CONFIG_LINGXIN_AUDIO_DOWN_CODEC "pcm"
#endif

static const char *TAG = "lx_sdk_bridge";

/* ---- AudioService record bridge ---- */

static std::atomic<bool> g_sdk_record_mode{false};
static std::atomic<bool> g_sdk_uplink_active{false};
static std::atomic<bool> g_volume_command_handled_this_turn{false};
static std::atomic<bool> g_suppress_cloud_tts_after_volume{false};
static std::atomic<int> g_last_volume_command_target{-1};
static std::atomic<bool> g_standby_after_playback{false};
static std::atomic<bool> g_standby_exit_in_progress{false};

extern "C" void lingxin_recorder_finish_open(void *recorder_hdl);

void audio_service_start_record_to_sdk(void)
{
    ESP_LOGI(TAG, "SDK record mode: START");
    g_sdk_record_mode.store(true);
}

void audio_service_stop_record_to_sdk(void)
{
    ESP_LOGI(TAG, "SDK record mode: STOP");
    g_sdk_record_mode.store(false);
#if CONFIG_USE_AUDIO_PROCESSOR
    audio_service_set_processor_task_priority(3);
#endif
}

int lingxin_sdk_is_record_mode(void)
{
    return g_sdk_record_mode.load() ? 1 : 0;
}

int audio_service_is_sdk_uplink_active(void)
{
    return g_sdk_uplink_active.load() ? 1 : 0;
}

void audio_service_wait_playback_idle(void)
{
    int64_t t0 = esp_timer_get_time();
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    int wait_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "wait_playback_idle done in %d ms", wait_ms);
}

int audio_service_is_playback_busy(void)
{
    return Application::GetInstance().GetAudioService().IsPlaybackBusy() ? 1 : 0;
}

void audio_service_schedule_recorder_uplink_begin(void *recorder_hdl)
{
    if (recorder_hdl == nullptr) {
        return;
    }

    Application::GetInstance().Schedule([recorder_hdl]() {
        int64_t t0 = esp_timer_get_time();
        auto &audio_service = Application::GetInstance().GetAudioService();
        audio_service.WaitForPlaybackQueueEmpty();
        int wait_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
        ESP_LOGI(TAG, "recorder uplink: wait_playback %d ms", wait_ms);

        /* Start consumer task and expose ringbuf before enabling AFE uplink. */
        lingxin_recorder_finish_open(recorder_hdl);

        audio_service_start_record_to_sdk();
        g_sdk_uplink_active.store(true);
        audio_service.EnableVoiceProcessing(true);
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_service_set_processor_task_priority(5);
#endif

        auto &app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateListening &&
            app.GetDeviceState() != kDeviceStateConnecting) {
            app.SetDeviceState(kDeviceStateListening);
        }
    });
}

void audio_service_schedule_recorder_uplink_end(void)
{
    Application::GetInstance().Schedule([]() {
        g_sdk_uplink_active.store(false);
        g_sdk_record_mode.store(false);
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_service_set_processor_task_priority(3);
#endif
        Application::GetInstance().GetAudioService().EnableVoiceProcessing(false);
        ESP_LOGI(TAG, "recorder uplink: deactivated");
    });
}

/* ---- AudioService playback bridge ---- */

void audio_service_push_decode_packet(const uint8_t *data, int len, const char *codec, int sample_rate)
{
    if (lingxin_should_suppress_cloud_tts()) {
        static int64_t last_drop_log_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_drop_log_us >= 500 * 1000) {
            last_drop_log_us = now_us;
            ESP_LOGI(TAG, "Drop cloud TTS after conflicting volume reply");
        }
        return;
    }

    auto &audio_service = Application::GetInstance().GetAudioService();

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->codec = codec ? codec : CONFIG_LINGXIN_AUDIO_DOWN_CODEC;
    packet->sample_rate = sample_rate;
    packet->channels = 1;
    packet->bits_per_sample = 16;
    packet->frame_duration = 60;
    packet->payload.assign(data, data + len);

    if (!audio_service.PushPacketToDecodeQueue(std::move(packet))) {
        ESP_LOGW(TAG, "Decode queue full, dropping packet (%d bytes)", len);
    }
}

void audio_service_reset_decoder(void)
{
    auto &audio_service = Application::GetInstance().GetAudioService();
    audio_service.ResetDecoder();
}

void audio_service_begin_downlink_playback(void)
{
    auto &app = Application::GetInstance();
    auto &audio_service = app.GetAudioService();
    audio_service.BeginDownlinkPlayback();
    /* Stop AFE/wake immediately so MP3 decode is not starved before prebuffer plays */
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(false);
}

void lingxin_notify_downlink_playback_ready(void)
{
#if CONFIG_LINGXIN_SDK_ENABLE
    if (LingxinSdkProtocol::GetInstance()) {
        LingxinSdkProtocol::GetInstance()->OnDownlinkStarted();
    }
#endif
}

void audio_service_flush_playback_pending(void)
{
    Application::GetInstance().GetAudioService().FlushPlaybackPending();
}

void audio_service_set_processor_task_priority(int priority)
{
#if CONFIG_USE_AUDIO_PROCESSOR
    Application::GetInstance().GetAudioService().SetProcessorTaskPriority(static_cast<UBaseType_t>(priority));
#endif
}

/* ---- AudioService local sound bridge ---- */

void audio_service_play_local_sound(const char *audio_path)
{
    if (!audio_path) return;

    auto &audio_service = Application::GetInstance().GetAudioService();

    ESP_LOGI(TAG, "SDK local sound request: %s", audio_path);
    audio_service.PlaySound(audio_path);
}

void audio_service_set_output_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGW(TAG, "Set output volume ignored: no audio codec");
        return;
    }

    codec->SetOutputVolume(volume);
    ESP_LOGI(TAG, "Output volume set to %d", volume);
}

void lingxin_mark_volume_command_handled(int target_volume)
{
    g_last_volume_command_target.store(target_volume);
    g_volume_command_handled_this_turn.store(true);
}

int lingxin_volume_command_handled_this_turn(void)
{
    return g_volume_command_handled_this_turn.load() ? 1 : 0;
}

void lingxin_set_suppress_cloud_tts(int suppress)
{
    g_suppress_cloud_tts_after_volume.store(suppress != 0);
}

int lingxin_should_suppress_cloud_tts(void)
{
    return g_suppress_cloud_tts_after_volume.load() ? 1 : 0;
}

int lingxin_get_last_volume_command_target(void)
{
    return g_last_volume_command_target.load();
}

void lingxin_clear_volume_command_suppress(void)
{
    g_suppress_cloud_tts_after_volume.store(false);
    g_volume_command_handled_this_turn.store(false);
    g_last_volume_command_target.store(-1);
}

void lingxin_mark_standby_after_playback(void)
{
    g_standby_after_playback.store(true);
    g_standby_exit_in_progress.store(true);
}

int lingxin_standby_after_playback_pending(void)
{
    return g_standby_after_playback.load() ? 1 : 0;
}

void lingxin_clear_standby_after_playback(void)
{
    g_standby_after_playback.store(false);
}

void lingxin_set_standby_exit_in_progress(int in_progress)
{
    g_standby_exit_in_progress.store(in_progress != 0);
}

int lingxin_standby_exit_in_progress(void)
{
    return g_standby_exit_in_progress.load() ? 1 : 0;
}

/* ---- Device info bridge ---- */

const char *lingxin_bridge_get_device_name(void)
{
    static std::string device_name;
    if (device_name.empty()) {
#ifdef CONFIG_LINGXIN_DEVICE_NAME
        device_name = CONFIG_LINGXIN_DEVICE_NAME;
#else
        return "LINGXIN_ESP32S3";
#endif
    }
    return device_name.c_str();
}

const char *lingxin_bridge_get_device_version(void)
{
#ifdef CONFIG_LINGXIN_DEVICE_VERSION
    return CONFIG_LINGXIN_DEVICE_VERSION;
#else
    return "1.3.0";
#endif
}
