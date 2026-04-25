// Shared RPC protocol and handshake field definitions.
#ifndef REMOCOM_COMMON_H
#define REMOCOM_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <cjson/cJSON.h>

#define REMOCOM_RPC_PROTOCOL_VERSION 2

#define MSG_TYPE_HANDSHAKE "handshake"
#define MSG_TYPE_HANDSHAKE_ACK "handshake_ack"
#define MSG_TYPE_HANDSHAKE_REJECT "handshake_reject"
#define MSG_TYPE_HANDSHAKE_REQUIRED "handshake_required"
#define MSG_TYPE_TASK_REQUEST "task_request"
#define MSG_TYPE_TASK_ASSIGNMENT "task_assignment"
#define MSG_TYPE_TASK_RESULT "task_result"
#define MSG_TYPE_NO_TASK "no_task"
#define MSG_TYPE_REGISTER "register"
#define MSG_TYPE_HEARTBEAT "heartbeat"
#define MSG_TYPE_ACK "ack"
#define MSG_TYPE_TASK_ERROR "task_error"
#define MSG_TYPE_UNKNOWN "unknown"

#define MSG_PAYLOAD_WORKER_REGISTER "Hello from worker"
#define MSG_PAYLOAD_TASK_REQUEST "requesting work"
#define MSG_PAYLOAD_HEARTBEAT_ALIVE "alive"
#define MSG_PAYLOAD_HANDSHAKE_ACCEPTED "Handshake accepted"
#define MSG_PAYLOAD_HANDSHAKE_REQUIRED "Handshake required before registration"
#define MSG_PAYLOAD_WORKER_REGISTERED "Worker registered"
#define MSG_PAYLOAD_NO_TASK "No tasks available"
#define MSG_PAYLOAD_UNKNOWN_TYPE "Unknown message type"

#define RPC_KEY_TYPE "type"
#define RPC_KEY_PAYLOAD "payload"

#define TASK_KEY_SOURCE "source"
#define TASK_KEY_OBJECT "object"
#define TASK_KEY_OUTPUT "output"
#define TASK_KEY_FILES "files"
#define TASK_KEY_FLAGS "flags"
#define TASK_KEY_PATH "path"
#define TASK_KEY_KIND "kind"
#define TASK_KEY_SIZE "size"
#define TASK_KEY_STATUS "status"
#define TASK_KEY_EXIT_CODE "exit_code"
#define TASK_KEY_MESSAGE "message"
#define TASK_KEY_COMPILER_OUTPUT "compiler_output"
#define TASK_KEY_HAS_OBJECT "has_object"
#define TASK_KEY_OBJECT_SIZE "object_size"

#define TASK_FILE_KIND_SOURCE "source"
#define TASK_FILE_KIND_HEADER "header"

#define TASK_STATUS_SUCCESS "success"
#define TASK_STATUS_FAILURE "failure"

#define HANDSHAKE_KEY_GCC_VERSION "gcc_version"
#define HANDSHAKE_KEY_TARGET_ARCH "target_arch"
#define HANDSHAKE_KEY_TARGET_OS "target_os"
#define HANDSHAKE_KEY_RPC_PROTOCOL_VERSION "rpc_protocol_version"

/// @brief Detects the target architecture at compile time.
/// @return A string representing the target architecture.
const char *remocom_detect_target_arch(void);

/// @brief Detects the target operating system at compile time.
/// @return A string representing the target operating system.
const char *remocom_detect_target_os(void);

/// @brief Parses a JSON string into a uint64_t value.
/// @param value JSON string to parse.
/// @param out Destination for the parsed value.
/// @return 1 on success, 0 on failure.
int remocom_parse_u64_string(cJSON *value, uint64_t *out);

/// @brief Checks whether a source path should use the C++ compiler driver.
/// @param source_path Source path from a manifest or task payload.
/// @return 1 for common C++ source extensions, 0 otherwise.
int remocom_is_cpp_source_path(const char *source_path);

/// @brief Selects the compiler driver for a source file.
/// @return "g++" for C++ sources, otherwise "gcc".
const char *remocom_select_source_driver(const char *source_path);

/// @brief Captures process output from a file descriptor into a bounded buffer.
/// @return 1 if the stream was read successfully, 0 otherwise.
int remocom_read_process_output(int read_fd, char *output, size_t output_size);

/// @brief Runs a child process while capturing stdout and stderr.
/// @param argv Null-terminated argument vector; argv[0] is the executable.
/// @param output Buffer for captured output.
/// @param output_size Size of output buffer.
/// @param wait_status_out Destination for the raw waitpid status.
/// @return 1 if the child was launched, captured, and waited for successfully; 0 otherwise.
int remocom_run_process_capture(char *const argv[], char *output, size_t output_size, int *wait_status_out);

/// @brief Sends all bytes from a buffer to a file descriptor.
/// @return 1 on success, 0 on failure.
int remocom_send_all(int fd, const void *buf, size_t len);

/// @brief Receives an exact number of bytes from a file descriptor.
/// @return 1 on success, 0 on failure.
int remocom_recv_all(int fd, void *buf, size_t len);

/// @brief Sends a framed JSON message with an arbitrary cJSON payload.
/// @return 1 on success, 0 on failure.
int remocom_send_json_with_payload(int fd, const char *type, cJSON *payload);

/// @brief Sends a framed JSON message with a string payload.
/// @return 1 on success, 0 on failure.
int remocom_send_json_message(int fd, const char *type, const char *payload);

/// @brief Receives and parses one framed JSON message.
/// @return Parsed message object, or NULL on failure.
cJSON *remocom_recv_json_message(int fd);

/// @brief Gets the size of a regular file.
/// @return 1 on success, 0 on failure.
int remocom_get_file_size(const char *path, uint64_t *size_out);

/// @brief Sends file contents to a file descriptor as a raw stream.
/// @return 1 on success, 0 on failure.
int remocom_send_file_stream(int fd, const char *path);

/// @brief Receives a raw file stream and writes it to a path.
/// @return 1 on success, 0 on failure.
int remocom_recv_file_stream(int fd, const char *path, uint64_t size);

#endif
