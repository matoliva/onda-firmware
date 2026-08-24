#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "storage.h"

#define ONDA_STORAGE_MOUNT_PATH "/sdcard"
#define ONDA_STORAGE_TEST_PATH ONDA_STORAGE_MOUNT_PATH "/onda-test.txt"
#define ONDA_STORAGE_MAX_OPEN_FILES 1

#define ONDA_STORAGE_SD_CLK GPIO_NUM_39
#define ONDA_STORAGE_SD_CMD GPIO_NUM_41
#define ONDA_STORAGE_SD_D0 GPIO_NUM_40

static const char *TAG = "ONDA_STORAGE";
static const char TEST_CONTENT[] = "Onda SD card verification\n";

static sdmmc_card_t *s_card;

static esp_err_t storage_close_file(FILE *file, const char *operation)
{
    if (fclose(file) == 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s close failed: %s", operation, strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_init(void)
{
    if (s_card != NULL) {
        ESP_LOGW(TAG, "SD card is already mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing SD card");

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = ONDA_STORAGE_MAX_OPEN_FILES,
        .disk_status_check_enable = true,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = ONDA_STORAGE_SD_CLK;
    slot_config.cmd = ONDA_STORAGE_SD_CMD;
    slot_config.d0 = ONDA_STORAGE_SD_D0;

    const esp_err_t result = esp_vfs_fat_sdmmc_mount(ONDA_STORAGE_MOUNT_PATH,
                                                      &host,
                                                      &slot_config,
                                                      &mount_config,
                                                      &s_card);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to mount FAT filesystem or detect card: %s",
                 esp_err_to_name(result));
        s_card = NULL;
        return result;
    }

    ESP_LOGI(TAG, "Card mounted");
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t storage_verify(void)
{
    if (s_card == NULL) {
        ESP_LOGE(TAG, "Cannot verify an unmounted SD card");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t status_result = sdmmc_get_status(s_card);
    if (status_result != ESP_OK) {
        ESP_LOGE(TAG, "SD card is unavailable: %s", esp_err_to_name(status_result));
        return status_result;
    }

    const size_t expected_size = sizeof(TEST_CONTENT) - 1U;
    ESP_LOGI(TAG, "Writing %s", ONDA_STORAGE_TEST_PATH);
    FILE *file = fopen(ONDA_STORAGE_TEST_PATH, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to create test file: %s", strerror(errno));
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    const size_t bytes_written = fwrite(TEST_CONTENT, 1U, expected_size, file);
    if (bytes_written != expected_size || ferror(file)) {
        ESP_LOGE(TAG, "Failed to write test file: %s", strerror(errno));
        result = ESP_FAIL;
    } else if (fflush(file) != 0) {
        ESP_LOGE(TAG, "Failed to flush test file: %s", strerror(errno));
        result = ESP_FAIL;
    }

    const esp_err_t close_result = storage_close_file(file, "Test file write");
    if (result == ESP_OK && close_result != ESP_OK) {
        result = close_result;
    }
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(TAG, "Reading %s", ONDA_STORAGE_TEST_PATH);
    file = fopen(ONDA_STORAGE_TEST_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open test file for reading: %s", strerror(errno));
        return ESP_FAIL;
    }

    char read_content[sizeof(TEST_CONTENT)] = {0};
    const size_t bytes_read = fread(read_content, 1U, expected_size, file);
    if (bytes_read != expected_size || ferror(file)) {
        ESP_LOGE(TAG, "Failed to read expected test content: %s", strerror(errno));
        result = ESP_FAIL;
    } else {
        const int trailing_byte = fgetc(file);
        if (trailing_byte != EOF) {
            ESP_LOGE(TAG, "Test file has unexpected trailing data");
            result = ESP_FAIL;
        } else if (ferror(file)) {
            ESP_LOGE(TAG, "Failed to read test file end: %s", strerror(errno));
            result = ESP_FAIL;
        } else if (memcmp(read_content, TEST_CONTENT, expected_size) != 0) {
            ESP_LOGE(TAG, "Test file verification mismatch");
            result = ESP_FAIL;
        }
    }

    const esp_err_t read_close_result = storage_close_file(file, "Test file read");
    if (result == ESP_OK && read_close_result != ESP_OK) {
        result = read_close_result;
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Read verification successful");
    }
    return result;
}

esp_err_t storage_deinit(void)
{
    if (s_card == NULL) {
        return ESP_OK;
    }

    const esp_err_t result = esp_vfs_fat_sdcard_unmount(ONDA_STORAGE_MOUNT_PATH, s_card);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(result));
        return result;
    }

    s_card = NULL;
    ESP_LOGI(TAG, "Card unmounted");
    return ESP_OK;
}
