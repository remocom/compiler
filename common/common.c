#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REMOCOM_IO_CHUNK_SIZE 65536

/// @brief Ensures that all parent directories of a given path exist, creating them if necessary.
/// @param path The path for which to ensure parent directories exist.
/// @return 1 if successful, 0 otherwise.
static int remocom_ensure_parent_dirs(const char *path) {
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s", path);

    // Iterate through the path and create directories as needed.
    for (char *cursor = temp + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }

        // Temporarily terminate the string to get the parent directory path.
        // If the directory already exists, mkdir will fail with EEXIST, which we can ignore.
        *cursor = '\0';
        if (mkdir(temp, 0700) != 0 && errno != EEXIST) {
            return 0;
        }
        *cursor = '/';
    }

    return 1;
}

/// @brief Discards a specified number of bytes from a stream, reading and ignoring the data.
/// @param fd The file descriptor of the stream from which to discard data.
/// @param size The number of bytes to discard.
/// @return 1 if successful, 0 otherwise.
static int remocom_discard_stream(int fd, uint64_t size) {
    unsigned char buffer[REMOCOM_IO_CHUNK_SIZE];
    uint64_t remaining = size;

    while (remaining > 0) {
        size_t chunk_size = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);

        // Read and discard the data from the stream. If any read operation fails, return 0.
        if (!remocom_recv_all(fd, buffer, chunk_size)) {
            return 0;
        }
        remaining -= chunk_size;
    }

    return 1;
}

/// @brief Detects the target architecture at compile time and returns it as a string.
/// @return A pointer to the string representing the target architecture.
const char *remocom_detect_target_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

/// @brief Detects the target operating system at compile time and returns it as a string.
/// @return A pointer to the string representing the target operating system.
const char *remocom_detect_target_os(void) {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}

/// @brief Parses a JSON string into a uint64_t value.
/// @param value The JSON string to parse.
/// @param out Destination for the parsed value.
/// @return 1 if successful, 0 otherwise.
int remocom_parse_u64_string(cJSON *value, uint64_t *out) {
    if (!cJSON_IsString(value) || out == NULL) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value->valuestring, &end, 10);
    if (errno != 0 || end == value->valuestring || *end != '\0') {
        return 0;
    }

    *out = (uint64_t)parsed;
    return 1;
}

/// @brief Checks whether a source path should use the C++ compiler driver.
/// @param source_path Source path from the manifest or task payload.
/// @return 1 for common C++ source extensions, 0 otherwise.
int remocom_is_cpp_source_path(const char *source_path) {
    if (source_path == NULL) {
        return 0;
    }

    const char *extension = strrchr(source_path, '.');
    if (extension == NULL) {
        return 0;
    }

    return strcmp(extension, ".cpp") == 0 ||
        strcmp(extension, ".cc") == 0 ||
        strcmp(extension, ".cxx") == 0 ||
        strcmp(extension, ".C") == 0;
}

/// @brief Selects the compiler driver for a source file.
/// @return "g++" for C++ sources, otherwise "gcc".
const char *remocom_select_source_driver(const char *source_path) {
    return remocom_is_cpp_source_path(source_path) ? "g++" : "gcc";
}

/// @brief Captures child-process output and drains any bytes beyond the destination size.
/// @param read_fd The file descriptor from which to read child stdout/stderr.
/// @param output A buffer to store the captured output prefix.
/// @param output_size The size of the output buffer.
/// @return 1 if the stream was read successfully, 0 otherwise.
int remocom_read_process_output(int read_fd, char *output, size_t output_size) {
    size_t total = 0;
    int ok = 1;

    while (1) {
        char discard[512];
        char *target = discard;
        size_t capacity = sizeof(discard);
        int storing_output = 0;

        if (output != NULL && output_size > 0 && total + 1 < output_size) {
            target = output + total;
            capacity = output_size - total - 1;
            storing_output = 1;
        }

        ssize_t n = read(read_fd, target, capacity);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = 0;
            break;
        }
        if (n == 0) {
            break;
        }

        if (storing_output) {
            total += (size_t)n;
        }
    }

    if (output != NULL && output_size > 0) {
        output[total] = '\0';
    }
    return ok;
}

/// @brief Runs a child process, capturing stdout/stderr into a bounded buffer.
/// @param argv Null-terminated argument vector. argv[0] is used as the executable.
/// @param output Buffer for captured stdout/stderr.
/// @param output_size Size of output buffer.
/// @param wait_status_out Destination for the raw waitpid status.
/// @return 1 if the child was launched, captured, and waited for successfully; 0 otherwise.
int remocom_run_process_capture(
    char *const argv[],
    char *output,
    size_t output_size,
    int *wait_status_out
) {
    if (argv == NULL || argv[0] == NULL || wait_status_out == NULL) {
        errno = EINVAL;
        return 0;
    }
    if (output != NULL && output_size > 0) {
        output[0] = '\0';
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved_errno;
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            close(pipefd[1]);
            _exit(127);
        }
        close(pipefd[1]);

        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    int read_ok = remocom_read_process_output(pipefd[0], output, output_size);
    int read_errno = errno;
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return 0;
    }

    if (!read_ok) {
        errno = read_errno;
        return 0;
    }

    *wait_status_out = status;
    return 1;
}

/// @brief Sends all data from a buffer to a file descriptor.
/// @param fd The file descriptor to which to send data.
/// @param buf The buffer containing the data to send.
/// @param len The number of bytes to send.
/// @return 1 if successful, 0 otherwise.
int remocom_send_all(int fd, const void *buf, size_t len) {
    const unsigned char *cursor = (const unsigned char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, cursor + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // Retry if send was interrupted by a signal.
            }
            return 0;
        }
        if (n == 0) {
            return 0; // Connection closed by the peer.
        }
        sent += (size_t)n;
    }

    return 1;
}

/// @brief Receives all data from a file descriptor into a buffer.
/// @param fd The file descriptor from which to receive data.
/// @param buf The buffer into which to receive data.
/// @param len The number of bytes to receive.
/// @return 1 if successful, 0 otherwise.
int remocom_recv_all(int fd, void *buf, size_t len) {
    unsigned char *cursor = (unsigned char *)buf;
    size_t received = 0;

    while (received < len) {
        ssize_t n = recv(fd, cursor + received, len - received, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // Retry if recv was interrupted by a signal.
            }
            return 0;
        }
        if (n == 0) {
            return 0; // Connection closed by the peer.
        }
        received += (size_t)n;
    }

    return 1;
}

/// @brief Sends a JSON message with a payload to a file descriptor.
/// @param fd The file descriptor to which to send the message.
/// @param type The type of the message.
/// @param payload The JSON payload of the message.
/// @return 1 if successful, 0 otherwise.
int remocom_send_json_with_payload(int fd, const char *type, cJSON *payload) {
    cJSON *msg = cJSON_CreateObject();
    if (msg == NULL) {
        cJSON_Delete(payload);
        return 0;
    }

    cJSON_AddStringToObject(msg, "type", type);
    cJSON_AddItemToObject(msg, "payload", payload);

    char *json_string = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (json_string == NULL) {
        return 0;
    }

    size_t json_len = strlen(json_string);
    if (json_len > UINT32_MAX) {
        free(json_string);
        return 0;
    }

    // Send the length of the JSON string in network byte order, followed by the JSON string itself.
    uint32_t network_len = htonl((uint32_t)json_len);
    int ok = remocom_send_all(fd, &network_len, sizeof(network_len)) &&
        remocom_send_all(fd, json_string, json_len);

    free(json_string);
    return ok;
}

/// @brief Sends a JSON message with a string payload to a file descriptor.
/// @param fd The file descriptor to which to send the message.
/// @param type The type of the message.
/// @param payload The string payload of the message.
/// @return 1 if successful, 0 otherwise.
int remocom_send_json_message(int fd, const char *type, const char *payload) {
    cJSON *payload_value = cJSON_CreateString(payload != NULL ? payload : "");
    if (payload_value == NULL) {
        return 0;
    }

    return remocom_send_json_with_payload(fd, type, payload_value);
}

/// @brief Receives a JSON message from a file descriptor.
/// @param fd The file descriptor from which to receive the message.
/// @return The parsed JSON message, or NULL if an error occurred.
cJSON *remocom_recv_json_message(int fd) {
    uint32_t network_len = 0;
    if (!remocom_recv_all(fd, &network_len, sizeof(network_len))) {
        return NULL;
    }

    // Convert the length from network byte order to host byte order.
    uint32_t json_len = ntohl(network_len);
    if (json_len == 0) {
        return NULL;
    }

    char *json_string = (char *)malloc((size_t)json_len + 1);
    if (json_string == NULL) {
        return NULL;
    }

    if (!remocom_recv_all(fd, json_string, json_len)) {
        free(json_string);
        return NULL;
    }

    json_string[json_len] = '\0';
    cJSON *msg = cJSON_Parse(json_string);
    free(json_string);
    return msg;
}

/// @brief Gets the size of a file at a given path.
/// @param path The path to the file.
/// @param size_out A pointer to a uint64_t where the size will be stored.
/// @return 1 if successful, 0 otherwise.
int remocom_get_file_size(const char *path, uint64_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        return 0; // An error occurred or the path is not a regular file.
    }

    *size_out = (uint64_t)st.st_size;
    return 1;
}

/// @brief Sends the contents of a file at a given path to a file descriptor as a stream.
/// @param fd The file descriptor to which to send the file contents.
/// @param path The path to the file.
/// @return 1 if successful, 0 otherwise.
int remocom_send_file_stream(int fd, const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    unsigned char buffer[REMOCOM_IO_CHUNK_SIZE];
    int ok = 1;

    while (!feof(file)) {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n > 0 && !remocom_send_all(fd, buffer, n)) {
            ok = 0;
            break;
        }
        if (ferror(file)) {
            ok = 0;
            break;
        }
    }

    fclose(file);
    return ok;
}

/// @brief Receives a file stream from a file descriptor and writes it to a file at a given path.
/// @param fd The file descriptor from which to receive the stream.
/// @param path The path to the file where the stream will be written.
/// @param size The size of the stream to receive.
/// @return 1 if successful, 0 otherwise.
int remocom_recv_file_stream(int fd, const char *path, uint64_t size) {
    if (!remocom_ensure_parent_dirs(path)) {
        remocom_discard_stream(fd, size);
        return 0;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        remocom_discard_stream(fd, size);
        return 0;
    }

    unsigned char buffer[REMOCOM_IO_CHUNK_SIZE];
    uint64_t remaining = size;
    int ok = 1;

    while (remaining > 0) {
        size_t chunk_size = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (!remocom_recv_all(fd, buffer, chunk_size)) {
            ok = 0;
            break;
        }

        if (fwrite(buffer, 1, chunk_size, file) != chunk_size) {
            ok = 0;
            break;
        }

        remaining -= chunk_size;
    }

    if (fclose(file) != 0) {
        ok = 0;
    }

    return ok;
}
