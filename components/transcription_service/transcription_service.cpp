#include "transcription_service.h"

#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "esp_log.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_ai_service.h"

namespace transcription_service {
namespace {

constexpr const char* kTag = "TranscriptionSvc";
constexpr uint32_t kWorkerTaskStackWords = 8192;

struct TaskContext {
    recording_service::RecordedClipPtr clip = {};
};

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
bool s_request_in_flight = false;
int s_last_http_status = 0;
std::string s_last_status_message = {};
std::string s_last_error_code = {};
std::string s_last_error_message = {};
std::string s_last_transcript = {};

Snapshot BuildSnapshotLocked()
{
    Snapshot snapshot = {};
    snapshot.initialized = s_initialized;
    snapshot.provider_ready = local_ai_service::GetSnapshot().runtime.transcription_ready;
    snapshot.request_in_flight = s_request_in_flight;
    snapshot.last_http_status = s_last_http_status;
    snapshot.last_status_message = s_last_status_message;
    snapshot.last_error_code = s_last_error_code;
    snapshot.last_error_message = s_last_error_message;
    snapshot.last_transcript = s_last_transcript;
    return snapshot;
}

void NotifyLocked()
{
    EventHandler handler = s_event_handler;
    void* context = s_event_context;
    if (handler == nullptr) {
        return;
    }
    const Event event = {
        .snapshot = BuildSnapshotLocked(),
    };
    handler(event, context);
}

// Run local transcription on a worker and publish its result through snapshots/events.
void RunTranscription(void* raw_context)
{
    std::unique_ptr<TaskContext> context(static_cast<TaskContext*>(raw_context));
    if (!context || !context->clip || context->clip->empty()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_http_status = 0;
        s_last_status_message = "Transcription failed";
        s_last_error_code = "empty_audio";
        s_last_error_message = "No recorded audio available";
        s_last_transcript.clear();
        NotifyLocked();
        return;
    }

    const local_ai_service::TranscriptionResult result = local_ai_service::Transcribe(*context->clip);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_http_status = result.http_status;
        if (result.success) {
            s_last_status_message = "Transcript ready";
            s_last_error_code.clear();
            s_last_error_message.clear();
            s_last_transcript = result.transcript;
            ESP_LOGI(kTag,
                     "Local AI transcription succeeded: chars=%u wav_bytes=%u clip_ms=%u "
                     "upload_chunks=%u upload_elapsed_ms=%llu total_elapsed_ms=%llu",
                     static_cast<unsigned>(s_last_transcript.size()),
                     static_cast<unsigned>(result.wav_bytes),
                     static_cast<unsigned>(result.clip_duration_ms),
                     static_cast<unsigned>(result.upload_chunk_count),
                     static_cast<unsigned long long>(result.upload_elapsed_ms),
                     static_cast<unsigned long long>(result.total_elapsed_ms));
        } else {
            s_last_status_message = "Transcription failed";
            s_last_error_code = result.error_code;
            s_last_error_message = result.error_message;
            s_last_transcript.clear();
            ESP_LOGW(kTag, "Local AI transcription failed: http=%d code=%s message=%s",
                     result.http_status, s_last_error_code.c_str(), s_last_error_message.c_str());
        }
        NotifyLocked();
    }

}

void WorkerTask(void* context)
{
    RunTranscription(context);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) {
        return ESP_OK;
    }

    s_initialized = true;
    s_request_in_flight = false;
    s_last_http_status = 0;
    s_last_status_message = local_ai_service::GetSnapshot().runtime.transcription_ready
                                ? "Local AI ready for transcription"
                                : "Local AI transcription unavailable";
    s_last_error_code.clear();
    s_last_error_message.clear();
    s_last_transcript.clear();
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return BuildSnapshotLocked();
}

bool BeginTranscription(recording_service::RecordedClipPtr clip)
{
    if (Init() != ESP_OK) {
        return false;
    }

    const local_ai_service::Snapshot local_ai_snapshot = local_ai_service::GetSnapshot();

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_request_in_flight) {
            s_last_status_message = "Transcription already running";
            s_last_error_code = "request_in_flight";
            s_last_error_message = "A transcription request is already running";
            NotifyLocked();
            return false;
        }
        if (!local_ai_snapshot.runtime.transcription_ready) {
            s_last_http_status = 0;
            s_last_status_message = "Transcription unavailable";
            s_last_error_code = local_ai_snapshot.settings.configured ? "provider_not_ready"
                                                                    : "not_configured";
            s_last_error_message = local_ai_snapshot.settings.configured
                                       ? "Local AI is not ready yet"
                                       : "Local transcription server is not configured";
            s_last_transcript.clear();
            NotifyLocked();
            return false;
        }
        if (!clip || clip->empty()) {
            s_last_http_status = 0;
            s_last_status_message = "Transcription unavailable";
            s_last_error_code = "empty_audio";
            s_last_error_message = "No recorded audio available";
            s_last_transcript.clear();
            NotifyLocked();
            return false;
        }

        s_request_in_flight = true;
        s_last_http_status = 0;
        s_last_status_message = "Transcribing recording";
        s_last_error_code.clear();
        s_last_error_message.clear();
        s_last_transcript.clear();
        NotifyLocked();
    }

    TaskContext* task_context = new (std::nothrow) TaskContext();
    if (task_context == nullptr) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_status_message = "Transcription unavailable";
        s_last_error_code = "task_context_alloc_failed";
        s_last_error_message = "Failed to allocate transcription task context";
        NotifyLocked();
        return false;
    }
    task_context->clip = std::move(clip);

    const auto sample_count = task_context->clip->sample_count();
    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        WorkerTask, "transcription", kWorkerTaskStackWords, task_context,
        followup_task_config::kPriorityLocalAi, &task, followup_task_config::kSystemCore);
    if (created != pdPASS) {
        delete task_context;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_status_message = "Transcription unavailable";
        s_last_error_code = "task_start_failed";
        s_last_error_message = "Failed to queue transcription task";
        NotifyLocked();
        return false;
    }

    ESP_LOGI(kTag, "Starting Local AI transcription: samples=%u",
             static_cast<unsigned>(sample_count));
    return true;
}

}  // namespace transcription_service
