#ifndef REMOCOM_MANIFEST_LOADER_H
#define REMOCOM_MANIFEST_LOADER_H

#include <stddef.h>

#define REMOCOM_MAX_MANIFEST_VALUE 512
#define REMOCOM_MAX_FLAGS 64
#define REMOCOM_MAX_SOURCES 256
#define REMOCOM_MAX_HEADERS 256

/// @brief Represents the contents of a build manifest, including the output path,
/// compiler flags, source files, and header files.
typedef struct {
    char output[REMOCOM_MAX_MANIFEST_VALUE];
    char flags[REMOCOM_MAX_FLAGS][REMOCOM_MAX_MANIFEST_VALUE];
    int flag_count;
    char sources[REMOCOM_MAX_SOURCES][REMOCOM_MAX_MANIFEST_VALUE];
    int source_count;
    char headers[REMOCOM_MAX_HEADERS][REMOCOM_MAX_MANIFEST_VALUE];
    int header_count;
} BuildManifest;

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Loads and validates a build manifest from a TOML file.
/// @param manifest_path Path to manifest file.
/// @param manifest Output manifest struct to populate on success.
/// @param error_buf Buffer to receive error messages on failure.
/// @param error_buf_size Size of error buffer.
/// @return 1 on success, 0 on failure.
int remocom_load_manifest_file(
    const char *manifest_path,
    BuildManifest *manifest,
    char *error_buf,
    size_t error_buf_size
);

#ifdef __cplusplus
}
#endif

#endif
