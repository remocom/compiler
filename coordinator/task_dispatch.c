#include "task_dispatch.h"

#include <cjson/cJSON.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include "../common/common.h"

#define MAX_TRANSFER_FILES (REMOCOM_MAX_HEADERS + 1)
#define DEPENDENCY_OUTPUT_SIZE 65536
#define TASK_DISPATCH_LOG_SIZE 1024

/// @brief Represents a file that needs to be transferred to a worker.
typedef struct {
    const char *path;
    const char *kind;
    uint64_t size;
} TransferFile;

/// @brief Logs a message to the task dispatch context.
/// @param ctx The task dispatch context.
/// @param message The message to log.
static void task_dispatch_log(TaskDispatchContext *ctx, const char *message) {
    if (ctx != NULL && ctx->log_message != NULL) {
        ctx->log_message(ctx->callback_ctx, message);
    }
}

/// @brief Logs a formatted message to the task dispatch context.
/// @param ctx The task dispatch context.
/// @param format The format string for the message.
/// @param ... The arguments for the format string.
static void task_dispatch_logf(TaskDispatchContext *ctx, const char *format, ...) {
    char message[TASK_DISPATCH_LOG_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    task_dispatch_log(ctx, message);
}

/// @brief Adds a file to the list of files to be transferred.
/// @param transfer_files The array of transfer files to add to.
/// @param transfer_paths The array of paths corresponding to the transfer files.
/// @param transfer_file_count The current count of transfer files, updated if a file is added.
/// @param path The path of the file to add.
/// @param kind The kind of the file (e.g. "source" or "header").
/// @return 1 if the file was added or is already in the list, 0 if there was an error
/// (e.g. too many files).
static int add_transfer_file(
    TransferFile *transfer_files,
    char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE],
    int *transfer_file_count,
    const char *path,
    const char *kind
) {
    for (int i = 0; i < *transfer_file_count; i++) {
        if (strcmp(transfer_files[i].path, path) == 0) {
            return 1;
        }
    }

    if (*transfer_file_count >= MAX_TRANSFER_FILES) {
        return 0;
    }

    snprintf(transfer_paths[*transfer_file_count], MAX_MANIFEST_VALUE, "%s", path);
    transfer_files[*transfer_file_count].path = transfer_paths[*transfer_file_count];
    transfer_files[*transfer_file_count].kind = kind;
    transfer_files[*transfer_file_count].size = 0;
    (*transfer_file_count)++;
    return 1;
}

/// @brief Collects source and header dependencies for a compile task.
/// @param task The compile task to collect dependencies for.
/// @param transfer_files The array of transfer files to add to.
/// @param transfer_paths The array of paths corresponding to the transfer files.
/// @param transfer_file_count The current count of transfer files, updated if a file is added.
/// @param error_buf The buffer to store any error messages.
/// @param error_buf_size The size of the error buffer.
/// @return 1 if dependencies were collected successfully, 0 otherwise.
static int collect_source_dependencies(
    const CompileTask *task,
    TransferFile *transfer_files,
    char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE],
    int *transfer_file_count,
    char *error_buf,
    size_t error_buf_size
) {
    const char *compiler_driver = remocom_select_source_driver(task->source_path);
    char *argv[MAX_FLAGS + 4];
    int argc = 0;
    argv[argc++] = (char *)compiler_driver;
    for (int i = 0; i < task->flag_count; i++) {
        argv[argc++] = (char *)task->flags[i];
    }
    argv[argc++] = "-MM";
    argv[argc++] = (char *)task->source_path;
    argv[argc] = NULL;

    char output[DEPENDENCY_OUTPUT_SIZE] = {0};
    int status = 0;
    if (!remocom_run_process_capture(argv, output, sizeof(output), &status) ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(error_buf, error_buf_size, "Dependency scan failed for %s with %s: %s",
            task->source_path, compiler_driver, output);
        return 0;
    }

    char *deps = strchr(output, ':');
    if (deps == NULL) {
        snprintf(error_buf, error_buf_size, "Dependency scan output missing ':' for %s", task->source_path);
        return 0;
    }

    deps++;
    for (char *cursor = deps; *cursor != '\0'; cursor++) {
        if (*cursor == '\\' && (cursor[1] == '\n' || cursor[1] == '\r')) {
            *cursor = ' ';
        }
        if (*cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
            *cursor = ' ';
        }
    }

    char *token = strtok(deps, " ");
    while (token != NULL) {
        if (token[0] != '\0' && strcmp(token, "\\") != 0) {
            const char *kind = strcmp(token, task->source_path) == 0 ?
                TASK_FILE_KIND_SOURCE : TASK_FILE_KIND_HEADER;
            if (!add_transfer_file(transfer_files, transfer_paths, transfer_file_count, token, kind)) {
                snprintf(error_buf, error_buf_size, "Too many dependency files for %s", task->source_path);
                return 0;
            }
        }
        token = strtok(NULL, " ");
    }

    return 1;
}

/// @brief Pops the next compile task from the task queue in a thread-safe manner.
/// @param ctx The task dispatch context containing the task queue and synchronization primitives.
/// @param task Receives the next compile task if one is available.
/// @return 1 if a task was popped and stored in `task`, 0 if there are no more tasks available
/// at the moment.
static int pop_next_task(TaskDispatchContext *ctx, CompileTask *task) {
    int has_task = 0;

    pthread_mutex_lock(ctx->task_mutex);
    if (*ctx->next_task_index < *ctx->total_tasks) {
        *task = ctx->task_queue[*ctx->next_task_index];
        (*ctx->next_task_index)++;
        has_task = 1;
    }
    pthread_mutex_unlock(ctx->task_mutex);

    return has_task;
}

/// @brief Requeues a task for preparation failure.
/// @param ctx The task dispatch context.
/// @param task The compile task to requeue.
static void requeue_task_for_prepare_failure(TaskDispatchContext *ctx, const CompileTask *task) {
    remocom_enqueue_task_for_reassign(ctx, task);
}

/// @brief Prepares the transfer files for a compile task.
/// @param ctx The task dispatch context.
/// @param task The compile task to prepare transfer files for.
/// @param node_id The ID of the node preparing the transfer files.
/// @param transfer_files The array of transfer files to populate.
/// @param transfer_paths The array of paths corresponding to the transfer files.
/// @param transfer_file_count The current count of transfer files, updated if a file is added.
/// @param error_buf The buffer to store any error messages.
/// @param error_buf_size The size of the error buffer.
/// @return 1 if the transfer files were prepared successfully, 0 otherwise.
static int prepare_transfer_files(
    TaskDispatchContext *ctx,
    const CompileTask *task,
    int node_id,
    TransferFile *transfer_files,
    char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE],
    int *transfer_file_count,
    char *error_buf,
    size_t error_buf_size
) {
    *transfer_file_count = 0;

    if (!add_transfer_file(
        transfer_files,
        transfer_paths,
        transfer_file_count,
        task->source_path,
        TASK_FILE_KIND_SOURCE
    )) {
        snprintf(error_buf, error_buf_size, "Unable to queue source transfer");
        task_dispatch_logf(ctx, "[ERROR] Unable to queue source transfer | Node %d | Source: %s\n",
            node_id, task->source_path);
        return 0;
    }

    if (!collect_source_dependencies(task, transfer_files, transfer_paths, transfer_file_count,
        error_buf, error_buf_size)) {
        task_dispatch_logf(ctx, "[ERROR] %s\n", error_buf);
        return 0;
    }

    for (int i = 0; i < ctx->manifest->header_count; i++) {
        if (!add_transfer_file(transfer_files, transfer_paths, transfer_file_count,
            ctx->manifest->headers[i], TASK_FILE_KIND_HEADER)) {
            snprintf(error_buf, error_buf_size, "Too many transfer files");
            task_dispatch_logf(ctx, "[ERROR] Too many transfer files for task | Node %d | Source: %s\n",
                node_id, task->source_path);
            return 0;
        }
    }

    for (int i = 0; i < *transfer_file_count; i++) {
        if (!remocom_get_file_size(transfer_files[i].path, &transfer_files[i].size)) {
            snprintf(error_buf, error_buf_size, "Transfer file unavailable");
            task_dispatch_logf(ctx, "[ERROR] Transfer file unavailable for task | Node %d | File: %s\n",
                node_id, transfer_files[i].path);
            return 0;
        }
    }

    return 1;
}

/// @brief Builds a JSON payload for a task assignment message.
/// @param task The compile task for which to build the payload.
/// @param transfer_files The array of transfer files to include in the payload.
/// @param transfer_file_count The number of transfer files to include.
/// @return A pointer to the created JSON object, or NULL on failure.
static cJSON *build_task_assignment_payload(
    const CompileTask *task,
    const TransferFile *transfer_files,
    int transfer_file_count
) {
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(payload, TASK_KEY_SOURCE, task->source_path);
    cJSON_AddStringToObject(payload, TASK_KEY_OBJECT, task->object_path);
    cJSON_AddStringToObject(payload, TASK_KEY_OUTPUT, task->build_output);

    cJSON *files = cJSON_AddArrayToObject(payload, TASK_KEY_FILES);
    for (int i = 0; i < transfer_file_count; i++) {
        char size_text[32];
        snprintf(size_text, sizeof(size_text), "%" PRIu64, transfer_files[i].size);

        cJSON *file_payload = cJSON_CreateObject();
        cJSON_AddStringToObject(file_payload, TASK_KEY_PATH, transfer_files[i].path);
        cJSON_AddStringToObject(file_payload, TASK_KEY_KIND, transfer_files[i].kind);
        cJSON_AddStringToObject(file_payload, TASK_KEY_SIZE, size_text);
        cJSON_AddItemToArray(files, file_payload);
    }

    cJSON *flags = cJSON_AddArrayToObject(payload, TASK_KEY_FLAGS);
    for (int i = 0; i < task->flag_count; i++) {
        cJSON_AddItemToArray(flags, cJSON_CreateString(task->flags[i]));
    }

    return payload;
}

/// @brief Sends a task assignment message to a worker.
/// @param client_fd The file descriptor of the client to send the message to.
/// @param task The compile task to assign.
/// @param transfer_files The array of transfer files to include in the message.
/// @param transfer_file_count The number of transfer files to include.
/// @return 1 if the message was sent successfully, 0 otherwise.
static int send_task_assignment(
    int client_fd,
    const CompileTask *task,
    const TransferFile *transfer_files,
    int transfer_file_count
) {
    cJSON *payload = build_task_assignment_payload(task, transfer_files, transfer_file_count);
    if (payload == NULL) {
        return 0;
    }

    int sent_ok = remocom_send_json_with_payload(client_fd, MSG_TYPE_TASK_ASSIGNMENT, payload);
    for (int i = 0; sent_ok && i < transfer_file_count; i++) {
        sent_ok = remocom_send_file_stream(client_fd, transfer_files[i].path);
    }

    return sent_ok;
}

void remocom_enqueue_task_for_reassign(TaskDispatchContext *ctx, const CompileTask *task) {
    pthread_mutex_lock(ctx->task_mutex);
    if (*ctx->total_tasks < ctx->max_tasks) {
        ctx->task_queue[*ctx->total_tasks] = *task;
        (*ctx->total_tasks)++;
    } else {
        task_dispatch_logf(ctx,
            "[ERROR] Cannot re-enqueue task (queue full, MAX_TASKS = %d): source=%s - build will be incomplete\n",
            ctx->max_tasks, task->source_path);
    }
    pthread_mutex_unlock(ctx->task_mutex);
}

void remocom_assign_task_to_worker(TaskDispatchContext *ctx, int client_fd, int node_id) {
    CompileTask task;
    if (!pop_next_task(ctx, &task)) {
        remocom_send_json_message(client_fd, MSG_TYPE_NO_TASK, MSG_PAYLOAD_NO_TASK);
        return;
    }

    TransferFile transfer_files[MAX_TRANSFER_FILES];
    char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE];
    int transfer_file_count = 0;
    char error_message[1024];

    if (!prepare_transfer_files(ctx, &task, node_id, transfer_files, transfer_paths,
        &transfer_file_count, error_message, sizeof(error_message))) {
        requeue_task_for_prepare_failure(ctx, &task);
        remocom_send_json_message(client_fd, MSG_TYPE_TASK_ERROR, error_message);
        return;
    }

    if (ctx->mark_task_active != NULL) {
        ctx->mark_task_active(ctx->callback_ctx, client_fd, &task);
    }

    if (!send_task_assignment(client_fd, &task, transfer_files, transfer_file_count)) {
        task_dispatch_logf(ctx, "[ERROR] Failed sending source file to Node %d | Source: %s\n",
            node_id, task.source_path);
    }

    task_dispatch_logf(ctx, "TASK ASSIGNED | Node %d | Source: %s | Object: %s\n",
        node_id, task.source_path, task.object_path);
}
