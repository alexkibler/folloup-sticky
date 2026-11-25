#ifndef LOCAL_AI_SERVICE_H_
#define LOCAL_AI_SERVICE_H_

#include <cstdint>
#include <string>
#include "esp_err.h"
#include "esp_http_server.h"

namespace recording_service { class RecordedClip; }

namespace local_ai_service {

struct SettingsSnapshot {
    bool configured = false;
    bool has_stored_api_key = false;
    std::string transcription_url;
    std::string transcription_model;
    std::string ollama_url;
    std::string model_name;
};

struct RuntimeSnapshot {
    bool initialized = false;
    bool ready = false;
    bool transcription_ready = false;
    bool text_ready = false;
    bool request_in_flight = false;
    int last_http_status = 0;
    std::string last_status_message;
    std::string last_error_code;
    std::string last_error_message;
};

struct Snapshot { SettingsSnapshot settings; RuntimeSnapshot runtime; };
struct Event { Snapshot snapshot; };
using EventHandler = void (*)(const Event&, void*);

struct TextResult {
    bool success = false;
    int http_status = 0;
    std::string text;
    std::string error_code;
    std::string error_message;
};

struct TranscriptionResult {
    bool success = false;
    int http_status = 0;
    std::string transcript;
    std::string error_code;
    std::string error_message;
    uint32_t clip_duration_ms = 0;
    size_t wav_bytes = 0;
    uint32_t upload_chunk_count = 0;
    uint64_t upload_elapsed_ms = 0;
    uint64_t total_elapsed_ms = 0;
};

esp_err_t Init();
void SetEventHandler(EventHandler handler, void* context);
Snapshot GetSnapshot();
TextResult GenerateText(const std::string& prompt);
TranscriptionResult Transcribe(const recording_service::RecordedClip& clip);
void SetNetworkState(bool connected, bool access_point_mode);
void RegisterPortalRoutes(httpd_handle_t server);
bool BeginAuthentication();

}  // namespace local_ai_service
#endif
