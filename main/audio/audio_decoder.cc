#include "audio_decoder.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <utility>

#include <esp_log.h>

#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec_default.h"

#define TAG "AudioDecoder"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

static uint16_t ReadLe16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t ReadLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() {
    Reset();
}

void AudioDecoder::Reset() {
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
    std::lock_guard<std::mutex> resampler_lock(resampler_mutex_);
    CloseMp3Decoder();
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
    mp3_decoder_sample_rate_ = 0;
    mp3_decoder_channels_ = 1;
    resampler_src_rate_ = 0;
    resampler_dst_rate_ = 0;
}

bool AudioDecoder::DecodeMp3(const uint8_t *data, size_t len, std::vector<int16_t> &out_pcm, int &sample_rate) {
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);

    if (!OpenMp3Decoder()) {
        return false;
    }

    int max_out_size = 4096;
    if (mp3_decode_buf_.size() < static_cast<size_t>(max_out_size)) {
        mp3_decode_buf_.resize(max_out_size);
    }
    uint8_t *out_buf = mp3_decode_buf_.data();

    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(data);
    raw.len = static_cast<uint32_t>(len);
    raw.consumed = 0;
    raw.eos = false;

    std::vector<int16_t> all_pcm;
    int total_decoded = 0;
    int iterations = 0;
    int realloc_count = 0;
    const int kMaxIterations = 1000;
    const int kMaxReallocs = 4;

    while (raw.len > 0) {
        if (++iterations > kMaxIterations) {
            ESP_LOGE(TAG, "MP3 decode exceeded max iterations (%d), aborting", kMaxIterations);
            break;
        }

        esp_audio_simple_dec_out_t out_frame = {};
        out_frame.buffer = out_buf;
        out_frame.len = max_out_size;

        esp_audio_err_t ret = esp_audio_simple_dec_process(mp3_decoder_, &raw, &out_frame);

        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (++realloc_count > kMaxReallocs) {
                ESP_LOGE(TAG, "MP3 decode exceeded max reallocs (%d), aborting", kMaxReallocs);
                break;
            }
            max_out_size = static_cast<int>(out_frame.needed_size);
            mp3_decode_buf_.resize(max_out_size);
            out_buf = mp3_decode_buf_.data();
            continue;
        }

        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "MP3 decode error: %d, skipping remaining %u bytes", ret, raw.len);
            break;
        }

        if (raw.consumed == 0) {
            ESP_LOGW(TAG, "MP3 decode stalled (consumed=0), aborting");
            break;
        }

        if (out_frame.decoded_size > 0) {
            if (total_decoded == 0) {
                esp_audio_simple_dec_info_t dec_info = {};
                esp_audio_simple_dec_get_info(mp3_decoder_, &dec_info);
                mp3_decoder_sample_rate_ = dec_info.sample_rate;
                mp3_decoder_channels_ = dec_info.channel;
                ESP_LOGD(TAG, "MP3 audio info: sample_rate=%d channel=%d bits=%d",
                         dec_info.sample_rate, dec_info.channel, dec_info.bits_per_sample);
            }

            size_t samples = out_frame.decoded_size / sizeof(int16_t);
            const int16_t *pcm = reinterpret_cast<const int16_t *>(out_frame.buffer);

            if (mp3_decoder_channels_ == 2) {
                size_t frames = samples / 2;
                size_t old_size = all_pcm.size();
                all_pcm.resize(old_size + frames);
                for (size_t i = 0; i < frames; ++i) {
                    all_pcm[old_size + i] = pcm[i * 2];
                }
            } else {
                all_pcm.insert(all_pcm.end(), pcm, pcm + samples);
            }
            total_decoded += out_frame.decoded_size;
        }

        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }

    if (all_pcm.empty()) {
        return false;
    }

    out_pcm = std::move(all_pcm);
    sample_rate = mp3_decoder_sample_rate_ > 0 ? mp3_decoder_sample_rate_ : 16000;
    return true;
}

bool AudioDecoder::ConvertRawPcm(const uint8_t *data, size_t len,
                                 int sample_rate, int channels, int bits_per_sample,
                                 std::vector<int16_t> &out_pcm) {
    if (bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported PCM bit depth: %d", bits_per_sample);
        return false;
    }
    if (sample_rate <= 0) {
        ESP_LOGE(TAG, "Invalid PCM sample rate: %d", sample_rate);
        return false;
    }
    if (channels <= 0) {
        ESP_LOGE(TAG, "Invalid PCM channel count: %d", channels);
        return false;
    }

    size_t sample_count = len / sizeof(int16_t);
    if (sample_count == 0) {
        return false;
    }

    const int16_t *samples = reinterpret_cast<const int16_t *>(data);
    if (channels == 1) {
        out_pcm.assign(samples, samples + sample_count);
    } else {
        size_t frame_count = sample_count / channels;
        out_pcm.resize(frame_count);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            out_pcm[frame] = samples[frame * channels];
        }
    }

    return true;
}

bool AudioDecoder::ConvertWav(const uint8_t *data, size_t len,
                              std::vector<int16_t> &out_pcm, int &sample_rate) {
    if (len < 44 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV payload");
        return false;
    }

    size_t offset = 12;
    int channels = 0;
    int bits_per_sample = 0;
    const uint8_t *pcm_data = nullptr;
    size_t pcm_size = 0;

    while (offset + 8 <= len) {
        const uint8_t *chunk = data + offset;
        uint32_t chunk_size = ReadLe32(chunk + 4);
        size_t next_offset = offset + 8 + chunk_size + (chunk_size & 1);
        if (next_offset > len + 1) {
            ESP_LOGE(TAG, "Invalid WAV chunk size");
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk");
                return false;
            }
            uint16_t audio_format = ReadLe16(chunk + 8);
            channels = ReadLe16(chunk + 10);
            sample_rate = ReadLe32(chunk + 12);
            bits_per_sample = ReadLe16(chunk + 22);
            if (audio_format != 1) {
                ESP_LOGE(TAG, "Unsupported WAV format: %u", audio_format);
                return false;
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm_data = chunk + 8;
            pcm_size = std::min<size_t>(chunk_size, len - offset - 8);
        }

        offset = next_offset;
    }

    if (pcm_data == nullptr || sample_rate <= 0 || channels <= 0 || bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported WAV audio params rate=%d channels=%d bits=%d", sample_rate, channels, bits_per_sample);
        return false;
    }

    return ConvertRawPcm(pcm_data, pcm_size, sample_rate, channels, bits_per_sample, out_pcm);
}

bool AudioDecoder::Resample(std::vector<int16_t> &pcm, int src_rate, int dst_rate) {
    std::lock_guard<std::mutex> resampler_lock(resampler_mutex_);

    if (src_rate <= 0 || dst_rate <= 0) {
        ESP_LOGE(TAG, "Invalid resample rate src=%d dst=%d", src_rate, dst_rate);
        return false;
    }
    if (src_rate == dst_rate) {
        return true;
    }

    if (output_resampler_ != nullptr &&
        (resampler_src_rate_ != src_rate || resampler_dst_rate_ != dst_rate)) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }

    if (output_resampler_ == nullptr) {
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(src_rate, dst_rate, ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
            return false;
        }
        resampler_src_rate_ = src_rate;
        resampler_dst_rate_ = dst_rate;
    }

    if (pcm.empty()) {
        return true;
    }

    uint64_t expected_size = static_cast<uint64_t>(pcm.size()) * static_cast<uint64_t>(dst_rate) /
        static_cast<uint64_t>(src_rate);
    uint32_t target_size = static_cast<uint32_t>(expected_size + 2048);
    uint64_t hard_limit = static_cast<uint64_t>(pcm.size()) * 8 + 4096;
    if (target_size == 0 || target_size > hard_limit) {
        ESP_LOGE(TAG, "Refuse abnormal resample size: in=%u src=%d dst=%d out=%u",
                 static_cast<unsigned>(pcm.size()), src_rate, dst_rate, target_size);
        return false;
    }

    std::vector<int16_t> resampled(target_size);
    uint32_t actual_output = target_size;
    auto ret = esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)pcm.data(), pcm.size(),
                                       (esp_ae_sample_t)resampled.data(), &actual_output);
    if (ret != 0) {
        ESP_LOGE(TAG, "Resample failed, ret=%d in=%u out=%u", ret,
                 static_cast<unsigned>(pcm.size()), static_cast<unsigned>(actual_output));
        return false;
    }
    if (actual_output > target_size) {
        ESP_LOGE(TAG, "Resample overflow: actual=%u target=%u",
                 static_cast<unsigned>(actual_output), static_cast<unsigned>(target_size));
        return false;
    }
    resampled.resize(actual_output);
    pcm = std::move(resampled);
    return true;
}

bool AudioDecoder::OpenMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        return true;
    }

    if (!mp3_decoders_registered_) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        mp3_decoders_registered_ = true;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    dec_cfg.dec_cfg = nullptr;
    dec_cfg.cfg_size = 0;
    dec_cfg.use_frame_dec = false;

    esp_audio_err_t ret = esp_audio_simple_dec_open(&dec_cfg, &mp3_decoder_);
    if (ret != ESP_AUDIO_ERR_OK || mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder, error: %d", ret);
        return false;
    }
    mp3_decoder_sample_rate_ = 0;
    mp3_decoder_channels_ = 1;
    ESP_LOGI(TAG, "MP3 decoder opened successfully");
    return true;
}

void AudioDecoder::CloseMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        esp_audio_simple_dec_close(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_sample_rate_ = 0;
    mp3_decoder_channels_ = 1;
}
