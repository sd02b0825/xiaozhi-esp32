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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

        auto &app = Application::GetInstance();

#if CONFIG_LINGXIN_SDK_ENABLE
        auto *sdk = LingxinSdkProtocol::GetInstance();
        if (sdk && sdk->IsPaused()) {
            ESP_LOGI(TAG, "recorder uplink: SDK paused, skipping recorder_finish_open");
            return;
        }
#endif

        if (app.GetDeviceState() != kDeviceStateListening &&
            app.GetDeviceState() != kDeviceStateConnecting) {
            app.SetDeviceState(kDeviceStateListening);
        }

        // Create audio_processing_task which reads from ringbuf.
        // This must happen BEFORE we feed buffered audio so the ringbuf
        // has an active consumer and won't overflow.
        lingxin_recorder_finish_open(recorder_hdl);

#if CONFIG_LINGXIN_SDK_ENABLE
        // Voiceprint flow has already captured the user's speech before SDK starts.
        // Feed only that buffered audio, then explicitly end SDK input so the cloud
        // can leave INPUTING and produce the model response.
        auto& feed_pcm = app.GetVoiceprintFeedPcm();
        if (!feed_pcm.empty()) {
            auto *sdk = LingxinSdkProtocol::GetInstance();
            g_sdk_uplink_active.store(true);
            if (sdk) {
                ESP_LOGI(TAG, "Feeding %u voiceprint buffered samples after recorder ready",
                         (unsigned)feed_pcm.size());
                sdk->FeedBufferedAudio(feed_pcm);
                vTaskDelay(pdMS_TO_TICKS(100));
                sdk->FinishBufferedAudioInput();
            } else {
                ESP_LOGW(TAG, "No Lingxin SDK instance, cannot finish buffered input");
            }
            feed_pcm.clear();
            feed_pcm.shrink_to_fit();
            return;
        }

        // Voice mode: audio captured by traditional protocol, fed later via FeedBufferedAudio.
        // Don't start live SDK recording; recorder task waits for buffered data.
        if (app.IsChatModeVoice()) {
            ESP_LOGI(TAG, "Voice mode: skipping live recorder, waiting for buffered audio");
            return;
        }
#endif

        // Normal SDK flow without voiceprint cache: stream live microphone audio.
        ESP_LOGI(TAG, "No voiceprint buffer, starting live SDK recorder uplink");
        audio_service_start_record_to_sdk();
        g_sdk_uplink_active.store(true);
        audio_service.EnableVoiceProcessing(true);
#if CONFIG_USE_AUDIO_PROCESSOR
        audio_service_set_processor_task_priority(5);
#endif
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
        return;
    }

#if CONFIG_LINGXIN_SDK_ENABLE
    if (LingxinSdkProtocol::GetInstance()) {
        LingxinSdkProtocol::GetInstance()->OnDownlinkStarted();
    }
#endif
}

void audio_service_reset_decoder(void)
{
    auto &audio_service = Application::GetInstance().GetAudioService();
    audio_service.ResetDecoder();
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
