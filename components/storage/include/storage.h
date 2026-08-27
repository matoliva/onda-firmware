#ifndef ONDA_STORAGE_H
#define ONDA_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct storage_file storage_file_t;

#define STORAGE_RECORDING_PATH_MAX 96U

/** Mount the onboard microSD card at /sdcard. */
esp_err_t storage_init(void);

/**
 * Verify the mounted card by writing and reading Onda's fixed diagnostic file.
 * The caller must successfully call storage_init() first.
 */
esp_err_t storage_verify(void);

/** Open one Onda-owned file for binary writing. */
esp_err_t storage_file_create(const char *path, storage_file_t **file);

/** Open one closed Onda-owned file for bounded sequential binary reading. */
esp_err_t storage_file_open_read(const char *path, storage_file_t **file);

/** Open a new Onda-owned file without overwriting an existing file. */
esp_err_t storage_file_create_exclusive(const char *path, storage_file_t **file);

/** Write an exact bounded byte range to an open storage file. */
esp_err_t storage_file_write(storage_file_t *file, const void *data, size_t size);

/** Read at most buffer_size bytes from an open sequential reader; zero means EOF. */
esp_err_t storage_file_read_next(storage_file_t *file,
                                 void *buffer,
                                 size_t buffer_size,
                                 size_t *bytes_read);

/** Seek an open storage file to an absolute byte offset. */
esp_err_t storage_file_seek(storage_file_t *file, long offset);

/** Flush an open storage file's buffered data to the SD card. */
esp_err_t storage_file_sync(storage_file_t *file);

/** Close an open storage file and release its single-file slot. */
esp_err_t storage_file_close(storage_file_t *file);

/** Return the size of a closed Onda-owned file. */
esp_err_t storage_file_get_size(const char *path, size_t *size);

/**
 * Prepare a unique final recording path and its sibling staging path.
 * The daily parent directory is created when absent. ESP_ERR_NOT_FOUND means
 * either path already exists and the caller should try another filename.
 */
esp_err_t storage_recording_prepare_paths(const char *final_path,
                                          char *staging_path,
                                          size_t staging_path_size);

/** Publish a closed staging file without replacing an existing completed recording. */
esp_err_t storage_file_publish(const char *source, const char *destination);

/**
 * Replace an Onda-owned destination with a closed source file.
 * If a destination exists, it is restored if the replacement rename fails.
 */
esp_err_t storage_file_replace(const char *source, const char *destination);

/** Remove an Onda-owned file that is not open. */
esp_err_t storage_file_remove(const char *path);

/** Unmount the mounted microSD card and release its filesystem resources. */
esp_err_t storage_deinit(void);

/** Return whether a closed Onda-owned path exists. */
esp_err_t storage_file_exists(const char *path, bool *exists);

/** Read a closed Onda-owned file into a caller-owned bounded buffer. */
esp_err_t storage_file_read(const char *path, void *buffer, size_t buffer_size, size_t *bytes_read);

typedef esp_err_t (*storage_recording_entry_callback_t)(const char *path, void *context);

/** Enumerate regular files one directory below /sdcard/recordings. */
esp_err_t storage_recordings_iterate(storage_recording_entry_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif

#endif
