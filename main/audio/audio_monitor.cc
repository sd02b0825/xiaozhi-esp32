/**
 * @file audio_monitor.cc
 * @brief Audio monitoring service implementation
 */

#include "audio_monitor.h"
#include "board.h"
#include "system_info.h"

#include <esp_log.h>
#include <mbedtls/base64.h>
#include <cJSON.h>

#define TAG "AudioMonitor"

// Event bits for task synchronization
#define AUDIO_MONITOR_TASK_EXITED (1 << 0)

//==============================================================================
// AudioRingBuffer Implementation
//==============================================================================

AudioRingBuffer::AudioRingBuffer(size_t capacity)
    : buffer_(std::make_unique<int16_t[]>(capacity)), capacity_(capacity) {
    write_pos_ = 0;
    read_pos_ = 0;
}

AudioRingBuffer::~AudioRingBuffer() = default;

size_t AudioRingBuffer::Write(const int16_t* data, size_t count) {
    if (data == nullptr || count == 0) {
        return 0;
    }

    size_t write_idx = write_pos_.load(std::memory_order_relaxed);
    size_t read_idx = read_pos_.load(std::memory_order_acquire);

    // Calculate available space
    size_t available;
    if (write_idx >= read_idx) {
        available = capacity_ - (write_idx - read_idx) - 1;
    } else {
        available = read_idx - write_idx - 1;
    }

    // Limit write count to available space
    size_t to_write = (count < available) ? count : available;
    if (to_write == 0) {
        return 0;
    }

    // Write data (may wrap around)
    size_t first_chunk = capacity_ - write_idx;
    if (first_chunk >= to_write) {
        // No wrap needed
        memcpy(buffer_.get() + write_idx, data, to_write * sizeof(int16_t));
        write_idx += to_write;
    } else {
        // Wrap around
        memcpy(buffer_.get() + write_idx, data, first_chunk * sizeof(int16_t));
        memcpy(buffer_.get(), data + first_chunk,
               (to_write - first_chunk) * sizeof(int16_t));
        write_idx = to_write - first_chunk;
    }

    // Ensure write index wraps properly
    if (write_idx >= capacity_) {
        write_idx -= capacity_;
    }

    write_pos_.store(write_idx, std::memory_order_release);
    return to_write;
}

size_t AudioRingBuffer::ReadAvailable(std::vector<int16_t>& output, size_t max_samples) {
    // Acquire sync: ensure we see all buffer writes before this point
    size_t write_idx = write_pos_.load(std::memory_order_acquire);
    size_t read_idx = read_pos_.load(std::memory_order_relaxed);

    // Calculate available data
    size_t available;
    if (write_idx >= read_idx) {
        available = write_idx - read_idx;
    } else {
        available = capacity_ - read_idx + write_idx;
    }

    if (available == 0) {
        output.clear();
        return 0;
    }

    // Limit read to max_samples if specified
    size_t to_read = (max_samples > 0 && max_samples < available) ? max_samples : available;

    // Resize output buffer
    output.resize(to_read);

    // Read data (may wrap around)
    // Note: We're reading a snapshot of data that was available at the time of write_idx load
    // New writes during this read won't affect our snapshot
    size_t first_chunk = capacity_ - read_idx;
    if (first_chunk >= to_read) {
        // No wrap needed
        memcpy(output.data(), buffer_.get() + read_idx, to_read * sizeof(int16_t));
    } else {
        // Wrap around
        memcpy(output.data(), buffer_.get() + read_idx, first_chunk * sizeof(int16_t));
        memcpy(output.data() + first_chunk, buffer_.get(),
               (to_read - first_chunk) * sizeof(int16_t));
    }

    // Update read position atomically with release semantics
    // This makes our read visible to the producer (it can now overwrite this region)
    size_t new_read_idx = read_idx + to_read;
    if (new_read_idx >= capacity_) {
        new_read_idx -= capacity_;
    }
    read_pos_.store(new_read_idx, std::memory_order_release);

    return to_read;
}

void AudioRingBuffer::Clear() {
    read_pos_.store(write_pos_.load(std::memory_order_relaxed),
                    std::memory_order_release);
}

size_t AudioRingBuffer::Size() const {
    size_t write_idx = write_pos_.load(std::memory_order_relaxed);
    size_t read_idx = read_pos_.load(std::memory_order_relaxed);

    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    } else {
        return capacity_ - read_idx + write_idx;
    }
}

//==============================================================================
// AudioMonitor Implementation
//==============================================================================

AudioMonitor::AudioMonitor()
    : ring_buffer_(std::make_unique<AudioRingBuffer>(BUFFER_CAPACITY_SAMPLES)),
      client_id_(SystemInfo::GetMacAddress()) {
    event_group_ = xEventGroupCreate();
}

AudioMonitor::~AudioMonitor() {
    Stop();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void AudioMonitor::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (running_) {
        ESP_LOGW(TAG, "Audio monitor already running");
        return;
    }
    if(upload_url_.empty() ) {
        ESP_LOGE(TAG, "Audio monitor URL is empty");
        return;
    }
    if (upload_url_.length() < 9 || 
        (upload_url_.find("http://") != 0 && upload_url_.find("https://") != 0)) {
        ESP_LOGE(TAG, "Audio monitor URL format is invalid: %s (must start with http:// or https://)", 
                 upload_url_.c_str());
        return;
    }
    
    running_ = true;
    upload_count_ = 0;
    upload_error_count_ = 0;

    // Clear buffer before starting
    ring_buffer_->Clear();

    // Create background upload task
    BaseType_t result = xTaskCreate(
        [](void* arg) {
            static_cast<AudioMonitor*>(arg)->UploadTask();
        },
        "audio_monitor",
        UPLOAD_TASK_STACK_SIZE,
        this,
        UPLOAD_TASK_PRIORITY,
        &upload_task_handle_);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        running_ = false;
        return;
    }

    ESP_LOGI(TAG, "Audio monitor started, buffer: %u samples (~%u sec), URL: %s",
             static_cast<unsigned int>(BUFFER_CAPACITY_SAMPLES),
             static_cast<unsigned int>(BUFFER_CAPACITY_SAMPLES / SAMPLE_RATE),
             upload_url_.c_str());
}

void AudioMonitor::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!running_) {
        return;
    }

    // Signal task to stop
    running_ = false;

    // Wait for task to exit gracefully (max 5 seconds to allow HTTP to complete)
    if (upload_task_handle_ != nullptr) {
        // Clear the exit flag before waiting
        xEventGroupClearBits(event_group_, AUDIO_MONITOR_TASK_EXITED);

        // Wait for task to signal it has exited
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            AUDIO_MONITOR_TASK_EXITED,
            pdTRUE,   // Clear on exit
            pdFALSE,  // Wait for any bit
            pdMS_TO_TICKS(5000)  // 5 second timeout
        );

        if (!(bits & AUDIO_MONITOR_TASK_EXITED)) {
            // Task didn't exit in time, force delete
            ESP_LOGW(TAG, "Upload task did not exit gracefully, force deleting");
            vTaskDelete(upload_task_handle_);
        }

        upload_task_handle_ = nullptr;
    }

    // Tear down the reusable HTTP client. The upload task has now exited, so
    // http_ is no longer in use. Close() triggers an asynchronous disconnect
    // inside EspTcp; its ReceiveTask may invoke OnTcpDisconnected shortly
    // after Close() returns. We wait for the grace period before destroying
    // the object so the callback always runs on a valid HttpClient, avoiding
    // the use-after-free / IWDT crash.
    if (http_ != nullptr) {
        http_->Close();
        vTaskDelay(pdMS_TO_TICKS(HTTP_CLOSE_GRACE_MS));
        http_.reset();
    }

    // Clear buffer
    ring_buffer_->Clear();

    ESP_LOGI(TAG, "Audio monitor stopped (uploads: %u, errors: %u)",
             upload_count_.load(), upload_error_count_.load());
}

void AudioMonitor::Feed(const std::vector<int16_t>& pcm_data) {
    if (!running_ || pcm_data.empty()) {
        return;
    }

    // Write to ring buffer (lock-free for single producer)
    size_t written = ring_buffer_->Write(pcm_data.data(), pcm_data.size());
    if (written < pcm_data.size()) {
        ESP_LOGD(TAG, "Ring buffer full, dropped %u samples", static_cast<unsigned int>(pcm_data.size() - written));
    }
}

void AudioMonitor::UploadTask() {
    ESP_LOGI(TAG, "Upload task started");

    while (running_) {
        // Check every 100ms
        vTaskDelay(pdMS_TO_TICKS(100));

        if (!running_) {
            break;
        }

        // Trigger upload when buffer has enough data (>= UPLOAD_CHUNK_SAMPLES)
        // This avoids fixed-interval timing issues where HTTP latency causes data
        // accumulation and loss. Data-driven triggering ensures we upload as soon
        // as 3 seconds of audio is available, regardless of upload duration.
        size_t buffered_samples = ring_buffer_->Size();
        if (buffered_samples < UPLOAD_CHUNK_SAMPLES) {
            continue;
        }

        // Collect audio data to upload (limit to UPLOAD_CHUNK_SAMPLES per upload)
        std::vector<int16_t> data_to_upload;
        size_t samples = ring_buffer_->ReadAvailable(data_to_upload, UPLOAD_CHUNK_SAMPLES);

        if (samples == 0) {
            continue;
        }

        ESP_LOGD(TAG, "Uploading %u samples (%.1f seconds)",
                 static_cast<unsigned int>(samples), samples / 16000.0f);

        // Perform upload (this may take time but doesn't block audio feeding)
        // Check running_ before and after to enable early exit
        if (!running_) break;
        UploadAudio(data_to_upload);
    }

    ESP_LOGI(TAG, "Upload task exiting gracefully");

    // Signal that task has exited
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, AUDIO_MONITOR_TASK_EXITED);
    }

    // Task will be cleaned up by Stop() or deleted by RTOS
    vTaskDelete(NULL);
}

void AudioMonitor::UploadAudio(const std::vector<int16_t>& audio_data) {
    if (audio_data.empty()) {
        return;
    }

    // Check network availability
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "Network not available, skip upload");
        upload_error_count_++;
        return;
    }

    // Reuse the persistent HTTP client across uploads instead of creating and
    // destroying one per request. Destroying the client right after Close()
    // raced with EspTcp::ReceiveTask's async OnTcpDisconnected callback and
    // caused an IWDT crash (use-after-free on the client's mutex). Keeping the
    // client alive guarantees the callback always targets a valid object.
    if (http_ == nullptr) {
        http_ = network->CreateHttp(HTTP_TIMEOUT_MS);
        if (http_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            upload_error_count_++;
            return;
        }
    }

    // Convert PCM to base64
    std::string base64_data = PcmToBase64(audio_data);

    // Build JSON payload
    std::string json_body = BuildJsonPayload(base64_data);

    // Set HTTP headers
    http_->SetHeader("Content-Type", "application/json");
    http_->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());

    // Set request body
    http_->SetContent(std::move(json_body));

    // Send POST request
    if (!http_->Open("POST", upload_url_)) {
        ESP_LOGE(TAG, "HTTP request failed, error: 0x%x", http_->GetLastError());
        upload_error_count_++;
        // Connection not opened, no need to call Close()
        return;
    }

    // Check response status
    int status_code = http_->GetStatusCode();
    if (status_code == 200) {
        std::string response = http_->ReadAll();

        // Parse response to check success flag
        cJSON* root = cJSON_Parse(response.c_str());
        if (root != nullptr) {
            cJSON* success = cJSON_GetObjectItem(root, "success");
            if (cJSON_IsBool(success) && cJSON_IsTrue(success)) {
                upload_count_++;
                ESP_LOGI(TAG, "Upload success: %u samples (%.2f sec)",
                         static_cast<unsigned int>(audio_data.size()),
                         static_cast<float>(audio_data.size()) / SAMPLE_RATE);
            } else {
                cJSON* message = cJSON_GetObjectItem(root, "message");
                ESP_LOGW(TAG, "Upload rejected by server: %s",
                         cJSON_IsString(message) ? message->valuestring : "unknown");
                upload_error_count_++;
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Failed to parse server response: %s",
                     response.substr(0, 100).c_str());
            upload_error_count_++;
        }
    } else {
        ESP_LOGW(TAG, "HTTP error: status=%d", status_code);
        upload_error_count_++;
    }

    // Close the connection (but keep the client object alive for reuse).
    // The actual disconnect notification is delivered asynchronously by
    // EspTcp::ReceiveTask; since the client is not destroyed here, the
    // OnTcpDisconnected callback remains safe.
    http_->Close();
}

std::string AudioMonitor::PcmToBase64(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return "";
    }

    // Get raw PCM bytes
    const uint8_t* raw_data = reinterpret_cast<const uint8_t*>(pcm.data());
    size_t raw_len = pcm.size() * sizeof(int16_t);

    // Calculate base64 output length
    size_t base64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &base64_len, raw_data, raw_len);

    // Encode to base64
    std::string result(base64_len, '\0');
    size_t actual_len = 0;
    int ret = mbedtls_base64_encode(
        reinterpret_cast<uint8_t*>(result.data()),
        result.size(),
        &actual_len,
        raw_data,
        raw_len);

    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed, error: %d", ret);
        return "";
    }

    result.resize(actual_len);
    return result;
}

std::string AudioMonitor::BuildJsonPayload(const std::string& base64_data) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }

    cJSON_AddStringToObject(root, "client_id", client_id_.c_str());
    cJSON_AddStringToObject(root, "data", base64_data.c_str());

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);

    cJSON_free(json_str);
    cJSON_Delete(root);

    return result;
}
