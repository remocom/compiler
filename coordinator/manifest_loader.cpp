#include "manifest_loader.h"

#include <toml.hpp>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

/// @brief Copies a string to a buffer, checking for overflow.
/// @param value The string to copy.
/// @param out The destination buffer.
/// @param out_size The size of the destination buffer.
/// @param field_name The name of the field being copied.
/// @param error_buf The buffer to store any error messages.
/// @param error_buf_size The size of the error buffer.
/// @return 1 on success, 0 otherwise.
static int copy_string_to_buffer(
    const std::string &value,
    char *out,
    size_t out_size,
    const char *field_name,
    char *error_buf,
    size_t error_buf_size
) {
    if (value.size() + 1 > out_size) {
        std::snprintf(error_buf, error_buf_size, "Manifest error: [build].%s is too long", field_name);
        return 0;
    }

    std::snprintf(out, out_size, "%s", value.c_str());
    return 1;
}

extern "C" int remocom_load_manifest_file(
    const char *manifest_path,
    BuildManifest *manifest,
    char *error_buf,
    size_t error_buf_size
) {
    if (manifest_path == NULL || manifest == NULL || error_buf == NULL || error_buf_size == 0) {
        return 0;
    }

    std::memset(manifest, 0, sizeof(*manifest));

    try {
        toml::value root = toml::parse(std::string(manifest_path));
        
        if (!root.contains("build")) {
            std::snprintf(error_buf, error_buf_size, "Manifest error: [build] section is required");
            return 0;
        }

        const toml::value build = toml::find(root, "build");

        // Validate and copy output field.
        const std::string output = toml::find<std::string>(build, "output");
        if (!copy_string_to_buffer(output, manifest->output, sizeof(manifest->output),
            "output", error_buf, error_buf_size)) {
            return 0;
        }

        // Validate and copy flags array if present.
        if (build.contains("flags")) {
            const std::vector<std::string> flags = toml::find<std::vector<std::string>>(build, "flags");
            if (flags.size() > REMOCOM_MAX_FLAGS) {
                std::snprintf(error_buf, error_buf_size,
                    "Manifest error: [build].flags has too many entries (max %d)", REMOCOM_MAX_FLAGS);
                return 0;
            }

            manifest->flag_count = static_cast<int>(flags.size());
            for (size_t i = 0; i < flags.size(); i++) {
                if (!copy_string_to_buffer(flags[i], manifest->flags[i], REMOCOM_MAX_MANIFEST_VALUE,
                    "flags", error_buf, error_buf_size)) {
                    return 0;
                }
            }
        }

        // Validate and copy sources array.
        const std::vector<std::string> sources = toml::find<std::vector<std::string>>(build, "sources");
        if (sources.empty()) {
            std::snprintf(error_buf, error_buf_size,
                "Manifest error: [build].sources must contain at least one source");
            return 0;
        }

        if (sources.size() > REMOCOM_MAX_SOURCES) {
            std::snprintf(error_buf, error_buf_size,
                "Manifest error: [build].sources has too many entries (max %d)", REMOCOM_MAX_SOURCES);
            return 0;
        }

        manifest->source_count = static_cast<int>(sources.size());
        for (size_t i = 0; i < sources.size(); i++) {
            if (!copy_string_to_buffer(sources[i], manifest->sources[i], REMOCOM_MAX_MANIFEST_VALUE,
                "sources", error_buf, error_buf_size)) {
                return 0;
            }
        }

        // Validate and copy local/project headers array if present.
        if (build.contains("headers")) {
            const std::vector<std::string> headers = toml::find<std::vector<std::string>>(build, "headers");
            if (headers.size() > REMOCOM_MAX_HEADERS) {
                std::snprintf(error_buf, error_buf_size,
                    "Manifest error: [build].headers has too many entries (max %d)", REMOCOM_MAX_HEADERS);
                return 0;
            }

            manifest->header_count = static_cast<int>(headers.size());
            for (size_t i = 0; i < headers.size(); i++) {
                if (!copy_string_to_buffer(headers[i], manifest->headers[i], REMOCOM_MAX_MANIFEST_VALUE,
                    "headers", error_buf, error_buf_size)) {
                    return 0;
                }
            }
        }
    } catch (const std::exception &ex) {
        std::snprintf(error_buf, error_buf_size, "Manifest parse error: %s", ex.what());
        return 0;
    }

    return 1;
}
