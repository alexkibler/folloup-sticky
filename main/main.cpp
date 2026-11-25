#include "app_shell.h"
#include "device_console.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_shell::Run();
    const esp_err_t console_err = device_console::Init();
    if (console_err != ESP_OK) {
        ESP_LOGW("DeviceConsole", "USB diagnostics unavailable: %s", esp_err_to_name(console_err));
    }
}
