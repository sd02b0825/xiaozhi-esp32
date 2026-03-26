/**
 * @file audio_monitor.h
 * @brief Audio monitoring service for idle state environment sound upload
 *
 * This module continuously captures audio in idle state and uploads
 * environment sound to a remote server via HTTP POST every 3 seconds.
 */
#ifndef AUDIO_MONITOR_H
#define AUDIO_MONITOR_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include <memory>

/**
 * @brief Ring buffer for lock-free single-producer single-consumer audio data
 *
 * This implementation uses a fixed-size circular buffer with proper memory
 * ordering to ensure thread-safe operation between producer (Write) and
 * consumer (ReadAvailable). The design guarantees:
 * - Producer never blocks consumer and vice versa
 * - No data corruption under concurrent access
 * - Consumer may miss some data if not reading fast enough (acceptable for monitoring)
 * 
 * Memory ordering strategy:
 * - Producer: release on write_pos_ update (makes buffer writes visible)
 * - Consumer: acquire on write_pos_ read (sees buffer writes), release on read_pos_
 */
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity);
    ~AudioRingBuffer();

    /**
     * Write audio data to the buffer (producer thread only)
     * @param data Pointer to PCM samples
     * @param count Number of samples to write
     * @return Number of samples actually written
     */
    size_t Write(const int16_t* data, size_t count);

    /**
     * Read available data from the buffer (consumer thread only)
     * This reads up to max_samples or all available if max_samples is 0.
     * 
     * @param output Vector to store read data (will be cleared first)
     * @param max_samples Maximum samples to read (0 = read all available)
     * @return Number of samples read
     */
    size_t ReadAvailable(std::vector<int16_t>& output, size_t max_samples = 0);

    /**
     * Clear the buffer (safe to call from any thread, but typically from consumer)
     */
    void Clear();

    /**
     * Get approximate number of samples in buffer
     * Note: Result may be stale by the time it's used (for statistics only)
     */
    size_t Size() const;

    /**
     * Check if buffer is likely empty
     */
    bool IsEmpty() const { return Size() == 0; }

    /**
     * Get buffer capacity
     */
    size_t Capacity() const { return capacity_; }

private:
    std::unique_ptr<int16_t[]> buffer_;
    size_t capacity_;
    
    // Use relaxed atomics with explicit acquire/release semantics at critical points
    // write_pos_ is only written by producer, read by consumer
    // read_pos_ is only written by consumer, read by producer
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
};

/**
 * @brief Audio Monitor - Captures and uploads environment audio in idle state
 *
 * This class runs independently from the main audio streaming pipeline.
 * It captures raw PCM audio from the microphone callback and periodically
 * uploads base64-encoded audio data to a remote server via HTTP.
 */
class AudioMonitor {
public:
    AudioMonitor();
    ~AudioMonitor();

    /**
     * Start audio monitoring (called when entering idle state)
     */
    void Start();

    /**
     * Stop audio monitoring (called when leaving idle state)
     */
    void Stop();

    /**
     * Feed audio data from AudioInputTask callback
     * @param pcm_data Raw PCM audio data (16-bit, 16kHz mono)
     */
    void Feed(const std::vector<int16_t>& pcm_data);

    /**
     * Check if monitoring is currently active
     */
    bool IsRunning() const { return running_; }

    /**
     * Set upload URL (for configuration flexibility)
     */
    void SetUploadUrl(const std::string& url) { upload_url_ = url; }

    /**
     * Set client ID (for configuration flexibility)
     */
    void SetClientId(const std::string& client_id) { client_id_ = client_id; }

private:
    /**
     * Background task that periodically uploads accumulated audio
     */
    void UploadTask();

    /**
     * Perform HTTP upload of audio data
     * @param audio_data Raw PCM audio data to upload
     */
    void UploadAudio(const std::vector<int16_t>& audio_data);

    /**
     * Convert PCM data to base64 string
     * @param pcm Raw PCM data
     * @return Base64 encoded string
     */
    std::string PcmToBase64(const std::vector<int16_t>& pcm);

    /**
     * Construct JSON payload for HTTP request
     * @param base64_data Base64 encoded audio data
     * @return JSON string
     */
    std::string BuildJsonPayload(const std::string& base64_data);

    // Configuration constants
    static constexpr int UPLOAD_INTERVAL_MS = 3000;  // 3 seconds upload interval
    static constexpr int SAMPLE_RATE = 16000;         // 16kHz sample rate
    static constexpr size_t BUFFER_CAPACITY_SAMPLES =
        16000 * 5;  // 5 seconds ring buffer (fixed memory: ~160KB)
    static constexpr int UPLOAD_TASK_STACK_SIZE = 8192;  // Increased for HTTP + base64 operations
    static constexpr int UPLOAD_TASK_PRIORITY = 5;
    static constexpr int HTTP_TIMEOUT_MS = 10000;  // 10 seconds HTTP timeout

    // Default configuration
    static constexpr const char* DEFAULT_UPLOAD_URL = "https://ed7c5a2ce79946108b57afb32224df19--8091.ap-shanghai2.cloudstudio.club/upload/audio";
    static constexpr const char* DEFAULT_CLIENT_ID = "xiaozhi-esp32";

    // State
    std::atomic<bool> running_{false};
    TaskHandle_t upload_task_handle_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;

    // Synchronization for start/stop operations
    std::mutex state_mutex_;

    // Ring buffer for audio data (fixed memory allocation)
    std::unique_ptr<AudioRingBuffer> ring_buffer_;

    // Timing
    std::chrono::steady_clock::time_point last_upload_time_;

    // Configuration
    std::string upload_url_{DEFAULT_UPLOAD_URL};
    std::string client_id_{DEFAULT_CLIENT_ID};

    // Statistics (atomic for thread-safe access from multiple threads)
    std::atomic<uint32_t> upload_count_{0};
    std::atomic<uint32_t> upload_error_count_{0};
};

#endif  // AUDIO_MONITOR_H
