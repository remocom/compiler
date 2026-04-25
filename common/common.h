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

#define HANDSHAKE_KEY_GCC_VERSION "gcc_version"
#define HANDSHAKE_KEY_TARGET_ARCH "target_arch"
#define HANDSHAKE_KEY_TARGET_OS "target_os"
#define HANDSHAKE_KEY_RPC_PROTOCOL_VERSION "rpc_protocol_version"

const char *remocom_detect_target_arch(void);
const char *remocom_detect_target_os(void);

int remocom_send_all(int fd, const void *buf, size_t len);
int remocom_recv_all(int fd, void *buf, size_t len);
int remocom_send_json_with_payload(int fd, const char *type, cJSON *payload);
int remocom_send_json_message(int fd, const char *type, const char *payload);
cJSON *remocom_recv_json_message(int fd);
int remocom_get_file_size(const char *path, uint64_t *size_out);
int remocom_send_file_stream(int fd, const char *path);
int remocom_recv_file_stream(int fd, const char *path, uint64_t size);

#endif
