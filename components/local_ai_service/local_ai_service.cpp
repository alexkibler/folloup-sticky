#include "local_ai_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "recording_service.h"
#include "sdkconfig.h"

namespace local_ai_service {
namespace {
constexpr const char* kTag = "LocalAI";
constexpr const char* kNamespace = "local_ai";
constexpr size_t kMaxResponseBytes = 64 * 1024;
constexpr int kRequestTimeoutMs = 180000;
constexpr const char* kBoundary = "folloup-audio-7cb58b72";

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& body)
{
    return Json(cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
}
std::string Field(cJSON* root, const char* name)
{
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
std::string Encode(cJSON* root)
{
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "";
    cJSON_free(raw);
    return result;
}
std::string Trim(std::string value)
{
    const auto nonspace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonspace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonspace).base(), value.end());
    return value;
}

struct Settings {
    std::string transcription_url = CONFIG_FOLLOWUP_TRANSCRIPTION_URL;
    std::string transcription_model = CONFIG_FOLLOWUP_TRANSCRIPTION_MODEL;
    std::string api_key = CONFIG_FOLLOWUP_TRANSCRIPTION_API_KEY;
    std::string ollama_url = CONFIG_FOLLOWUP_OLLAMA_URL;
    std::string model = CONFIG_FOLLOWUP_OLLAMA_MODEL;
};
std::mutex s_mutex;
Settings s_settings;
RuntimeSnapshot s_runtime;
EventHandler s_handler = nullptr;
void* s_handler_context = nullptr;
bool s_network_connected = false;
bool s_probe_running = false;
uint32_t s_generation = 0;

std::string ReadNvs(nvs_handle_t handle, const char* key, const std::string& fallback)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) return fallback;
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) return fallback;
    value.resize(length - 1);
    return value;
}
Settings SettingsCopy()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_settings;
}
bool Configured(const Settings& settings)
{
    return !settings.transcription_url.empty() && !settings.transcription_model.empty() &&
           !settings.ollama_url.empty() && !settings.model.empty();
}
Snapshot SnapshotLocked()
{
    Snapshot snapshot;
    snapshot.settings.configured = Configured(s_settings);
    snapshot.settings.has_stored_api_key = !s_settings.api_key.empty();
    snapshot.settings.transcription_url = s_settings.transcription_url;
    snapshot.settings.transcription_model = s_settings.transcription_model;
    snapshot.settings.ollama_url = s_settings.ollama_url;
    snapshot.settings.model_name = s_settings.model;
    snapshot.runtime = s_runtime;
    return snapshot;
}
void Notify()
{
    EventHandler handler;
    void* context;
    Event event;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_handler;
        context = s_handler_context;
        event.snapshot = SnapshotLocked();
    }
    if (handler) handler(event, context);
}

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};
esp_err_t OnHttpEvent(esp_http_client_event_t* event)
{
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && response && event->data_len > 0) {
        if (response->body.size() + event->data_len > kMaxResponseBytes) {
            response->error = "Server response exceeded 64 KiB";
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}
using Client = std::unique_ptr<std::remove_pointer_t<esp_http_client_handle_t>,
                               decltype(&esp_http_client_cleanup)>;
Client NewClient(const std::string& url, const std::string& key, HttpResponse* response,
                 int timeout_ms = kRequestTimeoutMs)
{
    if (url.rfind("https://", 0) == 0 && std::time(nullptr) < 1704067200) {
        response->error = "Device time is not set; enable clock sync before HTTPS";
        return Client(nullptr, esp_http_client_cleanup);
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = timeout_ms;
    config.disable_auto_redirect = true;
    config.event_handler = OnHttpEvent;
    config.user_data = response;
    Client client(esp_http_client_init(&config), esp_http_client_cleanup);
    if (client) {
        esp_http_client_set_header(client.get(), "Accept", "application/json");
        if (!key.empty()) {
            const std::string auth = "Bearer " + key;
            esp_http_client_set_header(client.get(), "Authorization", auth.c_str());
        }
    } else {
        response->error = "Could not create HTTP client";
    }
    return client;
}
HttpResponse Request(const std::string& url, const std::string& key = {},
                     const std::string* body = nullptr, int timeout_ms = kRequestTimeoutMs)
{
    HttpResponse response;
    auto client = NewClient(url, key, &response, timeout_ms);
    if (!client) return response;
    if (body) {
        esp_http_client_set_method(client.get(), HTTP_METHOD_POST);
        esp_http_client_set_header(client.get(), "Content-Type", "application/json");
        esp_http_client_set_post_field(client.get(), body->data(), body->size());
    }
    const esp_err_t err = esp_http_client_perform(client.get());
    response.status = esp_http_client_get_status_code(client.get());
    if (err != ESP_OK && response.error.empty()) response.error = esp_err_to_name(err);
    return response;
}
std::string ErrorMessage(const HttpResponse& response)
{
    if (!response.error.empty()) return response.error;
    auto root = Parse(response.body);
    std::string message = Field(root.get(), "error");
    if (message.empty()) {
        auto* error = cJSON_GetObjectItemCaseSensitive(root.get(), "error");
        message = Field(error, "message");
    }
    if (message.empty()) message = Field(root.get(), "message");
    if (message.empty()) message = "Server returned HTTP " + std::to_string(response.status);
    return message.substr(0, 240);
}
std::string AudioBase(const Settings& settings)
{
    constexpr const char* suffix = "/v1/audio/transcriptions";
    const auto offset = settings.transcription_url.rfind(suffix);
    return offset == std::string::npos || offset + std::strlen(suffix) != settings.transcription_url.size()
        ? std::string{} : settings.transcription_url.substr(0, offset);
}
std::string Origin(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    return url.substr(0, url.find('/', scheme + 3));
}
std::string OllamaToken(const Settings& settings)
{
    // The public Diction gateway authenticates both services with one pairing
    // token. Never forward that token to an Ollama server on another origin.
    return Origin(settings.transcription_url) == Origin(settings.ollama_url)
        ? settings.api_key : std::string{};
}
bool HasModel(cJSON* root, const char* array_name, const char* key, const std::string& model)
{
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, array_name)) {
        if (Field(item, key) == model) return true;
    }
    return false;
}

struct ProbeContext { Settings settings; uint32_t generation; };
void RunProbe(const ProbeContext& context)
{
    const auto& settings = context.settings;
    if (settings.transcription_url.rfind("https://", 0) == 0 ||
        settings.ollama_url.rfind("https://", 0) == 0) {
        // Wi-Fi starts the SNTP worker at the same time as this probe. Allow
        // certificate time validation to use its result on a cold boot.
        for (int attempt = 0; attempt < 150 && std::time(nullptr) < 1704067200; ++attempt)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    const std::string base = AudioBase(settings);
    HttpResponse audio = Request(base + "/v1/models", settings.api_key, nullptr, 15000);
    auto audio_json = Parse(audio.body);
    bool audio_ready = audio.ok() && cJSON_IsObject(audio_json.get());
    // Diction's public model list does not authenticate the pairing token. Verify it
    // on its dedicated route. A generic Whisper server has no pairing capability.
    auto* capabilities = cJSON_GetObjectItemCaseSensitive(audio_json.get(), "capabilities");
    if (audio_ready && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "pairing"))) {
        audio = Request(base + "/v1/auth/key", settings.api_key, nullptr, 15000);
        audio_ready = audio.ok();
    }
    const HttpResponse text = Request(settings.ollama_url + "/api/tags", OllamaToken(settings), nullptr, 15000);
    auto text_json = Parse(text.body);
    const bool text_ready = text.ok() && HasModel(text_json.get(), "models", "name", settings.model);
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_probe_running = false;
        s_runtime.request_in_flight = false;
        stale = context.generation != s_generation;
        if (!stale) {
            s_runtime.transcription_ready = audio_ready;
            s_runtime.text_ready = text_ready;
            s_runtime.ready = audio_ready && text_ready;
            s_runtime.last_http_status = !audio_ready ? audio.status : text.status;
            s_runtime.last_status_message = s_runtime.ready ? "Transcription and Ollama ready" :
                audio_ready ? "Transcription ready; check Ollama" : "Check transcription server";
            s_runtime.last_error_code = s_runtime.ready ? "" : "server_not_ready";
            s_runtime.last_error_message = !audio_ready ? ErrorMessage(audio) :
                !text.ok() ? ErrorMessage(text) : !text_ready ? "Ollama model is not installed" : "";
        }
    }
    ESP_LOGI(kTag, "Server check: transcription=%d text=%d", audio_ready, text_ready);
    Notify();
    if (stale) BeginAuthentication();
}
void ProbeTask(void* arg)
{
    {
        std::unique_ptr<ProbeContext> context(static_cast<ProbeContext*>(arg));
        RunProbe(*context);
    }
    vTaskDelete(nullptr);
}

bool WriteAll(esp_http_client_handle_t client, const void* bytes, size_t length)
{
    const char* data = static_cast<const char*>(bytes);
    while (length) {
        const int written = esp_http_client_write(client, data, std::min<size_t>(length, 4096));
        if (written <= 0) return false;
        data += written;
        length -= written;
    }
    return true;
}
std::array<uint8_t, 44> WavHeader(const recording_service::RecordedClip& clip)
{
    std::array<uint8_t, 44> header = {};
    std::memcpy(header.data(), "RIFF", 4);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    std::memcpy(header.data() + 36, "data", 4);
    const auto put32 = [&](size_t pos, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) header[pos + i] = value >> (8 * i);
    };
    put32(4, 36 + clip.pcm16_byte_count());
    put32(16, 16);
    header[20] = 1;
    header[22] = 1;
    put32(24, clip.sample_rate_hz());
    put32(28, clip.sample_rate_hz() * 2);
    header[32] = 2;
    header[34] = 16;
    put32(40, clip.pcm16_byte_count());
    return header;
}

esp_err_t SendSnapshot(httpd_req_t* request, const char* message)
{
    const Snapshot snapshot = GetSnapshot();
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(root.get(), "success", true);
    cJSON_AddStringToObject(root.get(), "message", message);
    auto* settings = cJSON_AddObjectToObject(root.get(), "settings");
    cJSON_AddBoolToObject(settings, "configured", snapshot.settings.configured);
    cJSON_AddBoolToObject(settings, "has_key", snapshot.settings.has_stored_api_key);
    cJSON_AddStringToObject(settings, "transcription_url", snapshot.settings.transcription_url.c_str());
    cJSON_AddStringToObject(settings, "transcription_model", snapshot.settings.transcription_model.c_str());
    cJSON_AddStringToObject(settings, "ollama_url", snapshot.settings.ollama_url.c_str());
    cJSON_AddStringToObject(settings, "model_name", snapshot.settings.model_name.c_str());
    auto* runtime = cJSON_AddObjectToObject(root.get(), "runtime");
    cJSON_AddBoolToObject(runtime, "ready", snapshot.runtime.ready);
    cJSON_AddBoolToObject(runtime, "transcription_ready", snapshot.runtime.transcription_ready);
    cJSON_AddBoolToObject(runtime, "text_ready", snapshot.runtime.text_ready);
    cJSON_AddBoolToObject(runtime, "request_in_flight", snapshot.runtime.request_in_flight);
    cJSON_AddStringToObject(runtime, "last_status_message", snapshot.runtime.last_status_message.c_str());
    cJSON_AddStringToObject(runtime, "last_error_message", snapshot.runtime.last_error_message.c_str());
    const std::string body = Encode(root.get());
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body.data(), body.size());
}
bool ValidUrl(const std::string& value)
{
    const size_t start = value.rfind("https://", 0) == 0 ? 8 : value.rfind("http://", 0) == 0 ? 7 : 0;
    return start && value.size() > start && value[start] != '/' && value.size() <= 256 &&
        value.find_first_of("\r\n\t @?#\\") == std::string::npos;
}
bool SafeField(const std::string& value, size_t max_length)
{
    return value.size() <= max_length && std::none_of(value.begin(), value.end(),
        [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '"'; });
}
esp_err_t SettingsError(httpd_req_t* request, const char* status, const char* message)
{
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(root.get(), "success", false);
    cJSON_AddStringToObject(root.get(), "message", message);
    const std::string body = Encode(root.get());
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body.data(), body.size());
}
esp_err_t HandleGet(httpd_req_t* request) { return SendSnapshot(request, "Local AI settings loaded"); }
esp_err_t HandleSettings(httpd_req_t* request)
{
    if (request->content_len <= 0 || request->content_len > 2048)
        return SettingsError(request, HTTPD_400, "Invalid settings payload");
    std::string body(request->content_len, '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int read = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (read <= 0) return ESP_FAIL;
        offset += read;
    }
    auto root = Parse(body);
    if (!cJSON_IsObject(root.get()))
        return SettingsError(request, HTTPD_400, "Invalid JSON");
    // Serialize the entire read-modify-persist operation against probes and other settings writes.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        Settings next = s_settings;
        struct Entry { const char* json; const char* nvs; std::string* value; };
        const Entry entries[] = {
            {"transcription_url", "stt_url", &next.transcription_url},
            {"transcription_model", "stt_model", &next.transcription_model},
            {"api_key", "api_key", &next.api_key},
            {"ollama_url", "ollama_url", &next.ollama_url},
            {"model_name", "model", &next.model},
        };
        for (const auto& entry : entries) {
            auto* item = cJSON_GetObjectItemCaseSensitive(root.get(), entry.json);
            if (!item) continue;
            if (!cJSON_IsString(item) || !item->valuestring)
                return SettingsError(request, HTTPD_400, "Settings must be strings");
            *entry.value = Trim(item->valuestring);
        }
        while (!next.ollama_url.empty() && next.ollama_url.back() == '/') next.ollama_url.pop_back();
        if (!ValidUrl(next.transcription_url) || AudioBase(next).empty() ||
            !ValidUrl(next.ollama_url) || !SafeField(next.api_key, 512) ||
            !SafeField(next.transcription_model, 128) || next.transcription_model.empty() ||
            !SafeField(next.model, 128) || next.model.empty())
            return SettingsError(request, HTTPD_400, "Check server URLs and model names");
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            for (const auto& entry : entries) {
                err = nvs_set_str(handle, entry.nvs, entry.value->c_str());
                if (err != ESP_OK) break;
            }
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err != ESP_OK)
            return SettingsError(request, HTTPD_500, "Could not save settings");
        s_settings = std::move(next);
        ++s_generation;
        s_runtime.ready = s_runtime.transcription_ready = s_runtime.text_ready = false;
        s_runtime.request_in_flight = s_probe_running;
    }
    Notify();
    BeginAuthentication();
    return SendSnapshot(request, "Local AI settings saved");
}
esp_err_t HandleCheck(httpd_req_t* request)
{
    BeginAuthentication();
    return SendSnapshot(request, "Checking local servers");
}
}  // namespace

esp_err_t Init()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_runtime.initialized) return ESP_OK;
        nvs_handle_t handle = 0;
        if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
            s_settings.transcription_url = ReadNvs(handle, "stt_url", s_settings.transcription_url);
            s_settings.transcription_model = ReadNvs(handle, "stt_model", s_settings.transcription_model);
            s_settings.api_key = ReadNvs(handle, "api_key", s_settings.api_key);
            s_settings.ollama_url = ReadNvs(handle, "ollama_url", s_settings.ollama_url);
            s_settings.model = ReadNvs(handle, "model", s_settings.model);
            nvs_close(handle);
        }
        s_runtime.initialized = true;
        s_runtime.last_status_message = "Connect Wi-Fi to use local AI";
    }
    Notify();
    return ESP_OK;
}
void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_handler = handler;
    s_handler_context = context;
}
Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return SnapshotLocked();
}
bool BeginAuthentication()
{
    ProbeContext* context;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_network_connected || s_probe_running || !Configured(s_settings)) return false;
        context = new (std::nothrow) ProbeContext{s_settings, ++s_generation};
        if (!context) return false;
        s_probe_running = true;
        s_runtime.request_in_flight = true;
        s_runtime.last_status_message = "Checking local servers";
    }
    const auto created = xTaskCreatePinnedToCore(ProbeTask, "local_ai_check", 8192, context,
        followup_task_config::kPriorityLocalAi, nullptr, followup_task_config::kSystemCore);
    if (created != pdPASS) {
        delete context;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_probe_running = false;
        s_runtime.request_in_flight = false;
        s_runtime.last_status_message = "Could not start server check";
        return false;
    }
    return true;
}
void SetNetworkState(bool connected, bool)
{
    bool check = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        check = connected && !s_network_connected;
        s_network_connected = connected;
        if (!connected) {
            ++s_generation;
            s_runtime.ready = s_runtime.transcription_ready = s_runtime.text_ready = false;
            s_runtime.request_in_flight = s_probe_running;
            s_runtime.last_status_message = "Connect Wi-Fi to use local AI";
        }
    }
    if (check) BeginAuthentication();
    Notify();
}

TextResult GenerateText(const std::string& prompt)
{
    TextResult result;
    if (prompt.empty()) {
        result.error_code = "empty_prompt";
        result.error_message = "Prompt is empty";
        return result;
    }
    const Settings settings = SettingsCopy();
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "model", settings.model.c_str());
    cJSON_AddStringToObject(root.get(), "prompt", prompt.c_str());
    cJSON_AddBoolToObject(root.get(), "stream", false);
    auto* options = cJSON_AddObjectToObject(root.get(), "options");
    cJSON_AddNumberToObject(options, "num_ctx", 8192);
    cJSON_AddNumberToObject(options, "num_predict", 1024);
    cJSON_AddNumberToObject(options, "temperature", 0.2);
    const std::string body = Encode(root.get());
    const HttpResponse http = Request(settings.ollama_url + "/api/generate", OllamaToken(settings), &body);
    result.http_status = http.status;
    auto response = Parse(http.body);
    if (http.ok()) result.text = Trim(Field(response.get(), "response"));
    result.success = http.ok() && !result.text.empty();
    if (!result.success) {
        result.error_code = http.ok() ? "empty_response" : "ollama_error";
        result.error_message = http.ok() ? "Ollama returned no text" : ErrorMessage(http);
    }
    return result;
}

TranscriptionResult Transcribe(const recording_service::RecordedClip& clip)
{
    TranscriptionResult result;
    const int64_t started = esp_timer_get_time();
    result.wav_bytes = clip.wav_byte_count();
    result.clip_duration_ms = clip.duration_ms();
    if (clip.empty()) {
        result.error_code = "empty_audio";
        result.error_message = "No audio to transcribe";
        return result;
    }
    const Settings settings = SettingsCopy();
    const std::string prefix = std::string("--") + kBoundary +
        "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + settings.transcription_model +
        "\r\n--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson" +
        "\r\n--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"" +
        "\r\nContent-Type: audio/wav\r\n\r\n";
    const std::string suffix = std::string("\r\n--") + kBoundary + "--\r\n";
    const size_t length = prefix.size() + result.wav_bytes + suffix.size();
    if (length > INT_MAX) {
        result.error_code = "audio_too_large";
        result.error_message = "Recording is too large";
        return result;
    }
    HttpResponse http;
    auto client = NewClient(settings.transcription_url, settings.api_key, &http);
    if (client) {
        esp_http_client_set_user_data(client.get(), nullptr);
        esp_http_client_set_method(client.get(), HTTP_METHOD_POST);
        const std::string content_type = std::string("multipart/form-data; boundary=") + kBoundary;
        esp_http_client_set_header(client.get(), "Content-Type", content_type.c_str());
        const auto header = WavHeader(clip);
        bool ok = esp_http_client_open(client.get(), length) == ESP_OK &&
                  WriteAll(client.get(), prefix.data(), prefix.size()) &&
                  WriteAll(client.get(), header.data(), header.size());
        clip.ForEachChunk([&](const int16_t* data, size_t samples) {
            if (ok) {
                ok = WriteAll(client.get(), data, samples * sizeof(int16_t));
                ++result.upload_chunk_count;
            }
        });
        ok = ok && WriteAll(client.get(), suffix.data(), suffix.size());
        result.upload_elapsed_ms = (esp_timer_get_time() - started) / 1000;
        if (ok && esp_http_client_fetch_headers(client.get()) >= 0) {
            http.status = esp_http_client_get_status_code(client.get());
            // Streaming mode delivers the body through read(), not the perform() callback.
            std::array<char, 2048> buffer;
            while (true) {
                const int read = esp_http_client_read(client.get(), buffer.data(), buffer.size());
                if (read < 0) { http.error = "Could not read transcription response"; break; }
                if (read == 0) break;
                if (http.body.size() + read > kMaxResponseBytes) {
                    http.error = "Transcription response exceeded 64 KiB"; break;
                }
                http.body.append(buffer.data(), read);
            }
            if (http.error.empty() && !esp_http_client_is_complete_data_received(client.get()))
                http.error = "Incomplete transcription response";
        } else {
            http.error = "Audio upload failed; check Wi-Fi and the transcription server";
        }
    }
    result.http_status = http.status;
    auto root = Parse(http.body);
    if (http.ok()) result.transcript = Trim(Field(root.get(), "text"));
    result.success = http.ok() && !result.transcript.empty();
    if (!result.success) {
        result.error_code = http.ok() ? "empty_transcript" : "transcription_error";
        result.error_message = http.ok() ? "No speech was recognized" : ErrorMessage(http);
    }
    result.total_elapsed_ms = (esp_timer_get_time() - started) / 1000;
    return result;
}

void RegisterPortalRoutes(httpd_handle_t server)
{
    struct Route { const char* path; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); };
    const Route routes[] = {
        {"/api/settings/ai", HTTP_GET, HandleGet},
        {"/api/settings/ai", HTTP_POST, HandleSettings},
        {"/api/runtime/ai", HTTP_GET, HandleGet},
        {"/api/settings/ai/check", HTTP_POST, HandleCheck},
    };
    for (const auto& route : routes) {
        httpd_uri_t uri = {};
        uri.uri = route.path;
        uri.method = route.method;
        uri.handler = route.handler;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
}
}  // namespace local_ai_service
