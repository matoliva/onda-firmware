#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "sdkconfig.h"

#define BYTES_PER_MEBIBYTE (1024U * 1024U)
#define EXPECTED_FLASH_SIZE_MB 8U
#define EXPECTED_PSRAM_SIZE_MB 8U

static const char *TAG = "ONDA";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Onda firmware");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const unsigned major_revision = chip_info.revision / 100;
    const unsigned minor_revision = chip_info.revision % 100;
    ESP_LOGI(TAG,
             "Target: %s, cores: %u, silicon revision: v%u.%u",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             major_revision,
             minor_revision);

    bool hardware_matches = true;

    if (chip_info.model != CHIP_ESP32S3) {
        ESP_LOGE(TAG, "Unexpected chip model; ESP32-S3 required");
        hardware_matches = false;
    }

    uint32_t flash_size_bytes = 0;
    const esp_err_t flash_result = esp_flash_get_size(NULL, &flash_size_bytes);
    if (flash_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash size: %s", esp_err_to_name(flash_result));
        hardware_matches = false;
    } else {
        const uint32_t flash_size_mb = flash_size_bytes / BYTES_PER_MEBIBYTE;
        ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size_mb);

        if (flash_size_mb != EXPECTED_FLASH_SIZE_MB) {
            ESP_LOGE(TAG,
                     "Unexpected flash size; expected %u MB",
                     EXPECTED_FLASH_SIZE_MB);
            hardware_matches = false;
        }
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized");
        hardware_matches = false;
    } else {
        const size_t psram_size_mb = esp_psram_get_size() / BYTES_PER_MEBIBYTE;
        ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)psram_size_mb);

        if (psram_size_mb != EXPECTED_PSRAM_SIZE_MB) {
            ESP_LOGE(TAG,
                     "Unexpected PSRAM size; expected %u MB",
                     EXPECTED_PSRAM_SIZE_MB);
            hardware_matches = false;
        }
    }

    if (!hardware_matches) {
        ESP_LOGE(TAG, "Board bring-up checks failed");
        return;
    }

    ESP_LOGI(TAG, "Board bring-up complete");
}
