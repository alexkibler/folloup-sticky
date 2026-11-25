#include "device_console.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "local_ai_service.h"
#include "recording_service.h"
#include "recording_archive_service.h"
#include "storage_service.h"
#include "wifi_service.h"

namespace device_console {
namespace {
int Status(int, char**)
{
    const auto wifi = wifi_service::GetUiState();
    const auto sd = storage_service::GetSnapshot();
    const auto ai = local_ai_service::GetSnapshot();
    storage_service::StorageStats stats;
    storage_service::GetStorageStats(&stats);
    printf("uptime=%llds heap=%u min_heap=%u reset=%d\n",
           esp_timer_get_time() / 1000000, static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()), esp_reset_reason());
    printf("epoch=%lld\n", static_cast<long long>(std::time(nullptr)));
    printf("wifi=%d ssid=%s ip=%s ap=%s\n", wifi.connected, wifi.ssid.c_str(),
           wifi.ip_address.c_str(), wifi.ap_ssid.c_str());
    printf("sd_inserted=%d sd_mounted=%d total=%llu free=%llu error=%s\n",
           sd.inserted, sd.mounted, stats.total_bytes, stats.free_bytes,
           esp_err_to_name(sd.last_error));
    printf("transcription=%d ollama=%d ai_status=%s ai_error=%s\n",
           ai.runtime.transcription_ready, ai.runtime.text_ready,
           ai.runtime.last_status_message.c_str(), ai.runtime.last_error_message.c_str());
    return 0;
}
int Format(int argc, char** argv)
{
    if (argc != 2 || std::strcmp(argv[1], "ERASE") != 0) {
        puts("Use sd-format ERASE to erase and format the microSD card as FAT32.");
        return 1;
    }
    const auto err = storage_service::RequestFormatSdCard();
    printf("SD format queued: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}
int Join(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        puts("Usage: wifi-join \"SSID\" [\"password\"]");
        return 1;
    }
    const bool queued = wifi_service::ConnectToNetwork(argv[1], argc == 3 ? argv[2] : "", true);
    puts(queued ? "Wi-Fi connection queued" : "Could not queue Wi-Fi connection");
    return queued ? 0 : 1;
}
int Check(int, char**)
{
    puts(local_ai_service::BeginAuthentication() ? "Server check queued" : "Check Wi-Fi/settings, or wait for the current check");
    return 0;
}
int AiTest(int, char**)
{
    const auto result = local_ai_service::GenerateText("Reply with exactly: Local AI is working.");
    printf("Ollama test: success=%d http=%d text=%s error=%s\n", result.success,
           result.http_status, result.text.c_str(), result.error_message.c_str());
    return result.success ? 0 : 1;
}
int Capture(int, char**)
{
    if (recording_service::GetUiState().recording || recording_service::GetUiState().armed) {
        puts("A recording is already in progress.");
        return 1;
    }
    puts("Recording a five-second microphone test now.");
    const auto err = recording_service::RecordDebugClipToWav("/sdcard/microphone-test.wav", 0, 5000);
    if (err != ESP_OK) {
        printf("Capture failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    const auto clip = recording_service::GetRecordedClip();
    if (!clip) return 1;
    const auto saved = recording_archive_service::SaveClip(*clip);
    const auto result = local_ai_service::Transcribe(*clip);
    if (result.success && saved.clip_saved)
        recording_archive_service::SaveTranscript(saved.recording_id, result.transcript);
    printf("Microphone test: saved=%d transcription=%d http=%d text=%s error=%s\n",
           saved.clip_saved, result.success, result.http_status, result.transcript.c_str(),
           result.error_message.c_str());
    recording_service::DiscardClip();
    return result.success ? 0 : 1;
}
int Log(int argc, char** argv)
{
    if (argc != 3 || std::strlen(argv[2]) != 1 || argv[2][0] < '0' || argv[2][0] > '5') {
        puts("Usage: log TAG LEVEL (0=none, 1=error, 2=warn, 3=info, 4=debug, 5=verbose)");
        return 1;
    }
    esp_log_level_set(argv[1], static_cast<esp_log_level_t>(argv[2][0] - '0'));
    return 0;
}
}  // namespace

esp_err_t Init()
{
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "folloup> ";
    config.max_cmdline_length = 256;
    config.task_stack_size = 16384;
    const esp_console_dev_usb_serial_jtag_config_t hardware = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hardware, &config, &repl);
    if (err != ESP_OK) return err;
    esp_console_register_help_command();
    struct Command { const char* name; const char* help; int (*run)(int, char**); };
    const Command commands[] = {
        {"status", "Show Wi-Fi, SD card, local AI and heap status", Status},
        {"sd-format", "Erase the microSD card: sd-format ERASE", Format},
        {"wifi-join", "Connect and save Wi-Fi: wifi-join SSID [password]", Join},
        {"ai-check", "Check transcription and Ollama servers", Check},
        {"ai-test", "Run a short Ollama request", AiTest},
        {"capture-test", "Record, save and transcribe five seconds from the microphone", Capture},
        {"log", "Set runtime logging: log TAG LEVEL (0-5)", Log},
    };
    for (const auto& item : commands) {
        esp_console_cmd_t command = {};
        command.command = item.name;
        command.help = item.help;
        command.func = item.run;
        ESP_ERROR_CHECK(esp_console_cmd_register(&command));
    }
    return esp_console_start_repl(repl);
}
}  // namespace device_console
