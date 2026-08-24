#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "storage.h"

#define ONDA_STORAGE_MOUNT_PATH "/sdcard"
#define ONDA_STORAGE_TEST_PATH ONDA_STORAGE_MOUNT_PATH "/onda-test.txt"
#define ONDA_STORAGE_REPLACE_BACKUP_PATH ONDA_STORAGE_MOUNT_PATH "/test.wav.bak"
#define ONDA_STORAGE_RECORDINGS_PATH ONDA_STORAGE_MOUNT_PATH "/recordings"
#define ONDA_STORAGE_MAX_OPEN_FILES 1

#define ONDA_STORAGE_SD_CLK GPIO_NUM_39
#define ONDA_STORAGE_SD_CMD GPIO_NUM_41
#define ONDA_STORAGE_SD_D0 GPIO_NUM_40

struct storage_file {
    FILE *handle;
};

static const char *TAG = "ONDA_STORAGE";
static const char TEST_CONTENT[] = "Onda SD card verification\n";

static sdmmc_card_t *s_card;
static storage_file_t s_open_file;

static esp_err_t storage_close_handle(FILE *file, const char *operation)
{
    if (fclose(file) == 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s close failed: %s", operation, strerror(errno));
    return ESP_FAIL;
}

static bool storage_path_exists(const char *path)
{
    struct stat status;
    return stat(path, &status) == 0;
}

static esp_err_t storage_check_mounted(void)
{
    if (s_card == NULL) {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t status_result = sdmmc_get_status(s_card);
    if (status_result != ESP_OK) {
        ESP_LOGE(TAG, "SD card is unavailable: %s", esp_err_to_name(status_result));
    }
    return status_result;
}

static esp_err_t storage_create_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
    return ESP_FAIL;
}

static esp_err_t storage_prepare_recording_directory(const char *final_path)
{
    const size_t root_length = strlen(ONDA_STORAGE_RECORDINGS_PATH);
    if (strncmp(final_path, ONDA_STORAGE_RECORDINGS_PATH, root_length) != 0 ||
        final_path[root_length] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    char directory[STORAGE_RECORDING_PATH_MAX];
    const char *const final_separator = strrchr(final_path, '/');
    if (final_separator == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t directory_length = (size_t)(final_separator - final_path);
    if (directory_length >= sizeof(directory)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(directory, final_path, directory_length);
    directory[directory_length] = '\0';
    esp_err_t result = storage_create_directory(ONDA_STORAGE_RECORDINGS_PATH);
    if (result == ESP_OK) {
        result = storage_create_directory(directory);
    }
    return result;
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

    const esp_err_t close_result = storage_close_handle(file, "Test file write");
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

    const esp_err_t read_close_result = storage_close_handle(file, "Test file read");
    if (result == ESP_OK && read_close_result != ESP_OK) {
        result = read_close_result;
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Read verification successful");
    }
    return result;
}

esp_err_t storage_file_create(const char *path, storage_file_t **file)
{
    if (path == NULL || file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t status_result = storage_check_mounted();
    if (status_result != ESP_OK) {
        return status_result;
    }
    if (s_open_file.handle != NULL) {
        ESP_LOGE(TAG, "A storage file is already open");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *handle = fopen(path, "wb");
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to create %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    s_open_file.handle = handle;
    *file = &s_open_file;
    return ESP_OK;
}

esp_err_t storage_file_create_exclusive(const char *path, storage_file_t **file)
{
    if (path == NULL || file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t status_result = storage_check_mounted();
    if (status_result != ESP_OK) {
        return status_result;
    }
    if (s_open_file.handle != NULL) {
        ESP_LOGE(TAG, "A storage file is already open");
        return ESP_ERR_INVALID_STATE;
    }

    const int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Failed to exclusively create %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    FILE *handle = fdopen(descriptor, "wb");
    if (handle == NULL) {
        const int open_errno = errno;
        close(descriptor);
        (void)remove(path);
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(open_errno));
        return ESP_FAIL;
    }

    s_open_file.handle = handle;
    *file = &s_open_file;
    return ESP_OK;
}

esp_err_t storage_file_write(storage_file_t *file, const void *data, size_t size)
{
    if (file == NULL || data == NULL || size == 0U || file != &s_open_file ||
        file->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_written = fwrite(data, 1U, size, file->handle);
    if (bytes_written == size && !ferror(file->handle)) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to write file data: %s", strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_file_seek(storage_file_t *file, long offset)
{
    if (file == NULL || file != &s_open_file || file->handle == NULL || offset < 0L) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(file->handle, offset, SEEK_SET) == 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to seek file: %s", strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_file_sync(storage_file_t *file)
{
    if (file == NULL || file != &s_open_file || file->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fflush(file->handle) == 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to flush file: %s", strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_file_close(storage_file_t *file)
{
    if (file == NULL || file != &s_open_file || file->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *handle = file->handle;
    file->handle = NULL;
    return storage_close_handle(handle, "Recording file");
}

esp_err_t storage_file_get_size(const char *path, size_t *size)
{
    if (path == NULL || size == NULL || s_open_file.handle != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat status;
    if (stat(path, &status) != 0 || status.st_size < 0) {
        ESP_LOGE(TAG, "Failed to inspect %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    *size = (size_t)status.st_size;
    return ESP_OK;
}

esp_err_t storage_recording_prepare_paths(const char *final_path,
                                          char *staging_path,
                                          size_t staging_path_size)
{
    if (final_path == NULL || staging_path == NULL || staging_path_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t status_result = storage_check_mounted();
    if (status_result != ESP_OK) {
        return status_result;
    }
    if (s_open_file.handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int staging_length = snprintf(staging_path, staging_path_size, "%s.part", final_path);
    if (staging_length < 0 || (size_t)staging_length >= staging_path_size) {
        staging_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t directory_result = storage_prepare_recording_directory(final_path);
    if (directory_result != ESP_OK) {
        return directory_result;
    }
    if (storage_path_exists(final_path) || storage_path_exists(staging_path)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t storage_file_publish(const char *source, const char *destination)
{
    if (source == NULL || destination == NULL || s_open_file.handle != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_path_exists(destination)) {
        ESP_LOGE(TAG, "Refusing to overwrite completed recording %s", destination);
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(source, destination) == 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to publish recording: %s", strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_file_replace(const char *source, const char *destination)
{
    if (source == NULL || destination == NULL || s_open_file.handle != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool has_destination = storage_path_exists(destination);
    if (storage_path_exists(ONDA_STORAGE_REPLACE_BACKUP_PATH)) {
        if (!has_destination || remove(ONDA_STORAGE_REPLACE_BACKUP_PATH) != 0) {
            ESP_LOGE(TAG, "Refusing to replace while %s exists", ONDA_STORAGE_REPLACE_BACKUP_PATH);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "Removed stale previous-recording backup");
    }
    if (has_destination) {
        if (rename(destination, ONDA_STORAGE_REPLACE_BACKUP_PATH) != 0) {
            ESP_LOGE(TAG, "Failed to stage previous recording: %s", strerror(errno));
            return ESP_FAIL;
        }
    }

    if (rename(source, destination) == 0) {
        if (has_destination && remove(ONDA_STORAGE_REPLACE_BACKUP_PATH) != 0) {
            ESP_LOGW(TAG, "New recording saved but backup cleanup failed: %s", strerror(errno));
        }
        return ESP_OK;
    }

    const int replace_errno = errno;
    if (has_destination && rename(ONDA_STORAGE_REPLACE_BACKUP_PATH, destination) != 0) {
        ESP_LOGE(TAG, "Failed to restore previous recording: %s", strerror(errno));
    }
    ESP_LOGE(TAG, "Failed to replace recording: %s", strerror(replace_errno));
    return ESP_FAIL;
}

esp_err_t storage_file_remove(const char *path)
{
    if (path == NULL || s_open_file.handle != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to remove %s: %s", path, strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_deinit(void)
{
    if (s_open_file.handle != NULL) {
        ESP_LOGE(TAG, "Cannot unmount SD card with an open file");
        return ESP_ERR_INVALID_STATE;
    }
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
