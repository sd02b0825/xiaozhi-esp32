#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "esp_audio_simple_dec.h"
#include "esp_ae_rate_cvt.h"

struct AudioStreamPacket;

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    bool DecodeMp3(const uint8_t *data, size_t len, std::vector<int16_t> &out_pcm, int &sample_rate);
    bool ConvertRawPcm(const uint8_t *data, size_t len,
                       int sample_rate, int channels, int bits_per_sample,
                       std::vector<int16_t> &out_pcm);
    bool ConvertWav(const uint8_t *data, size_t len,
                    std::vector<int16_t> &out_pcm, int &sample_rate);
    bool Resample(std::vector<int16_t> &pcm, int src_rate, int dst_rate);
    void Reset();

private:
    esp_audio_simple_dec_handle_t mp3_decoder_ = nullptr;
    bool mp3_decoders_registered_ = false;
    int mp3_decoder_sample_rate_ = 0;
    int mp3_decoder_channels_ = 1;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    int resampler_src_rate_ = 0;
    int resampler_dst_rate_ = 0;
    std::mutex decoder_mutex_;
    std::mutex resampler_mutex_;
    /* Reused across DecodeMp3 calls to avoid per-packet heap alloc stalls. */
    std::vector<uint8_t> mp3_decode_buf_;

    bool OpenMp3Decoder();
    void CloseMp3Decoder();
};

#endif /* AUDIO_DECODER_H */
