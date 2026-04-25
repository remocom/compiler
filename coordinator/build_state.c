#include "build_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BUILD_STATE_LOG_SIZE 1024

/// @brief Logs a message to the build state context.
/// @param ctx The build state context.
/// @param message The message to log.
static void build_state_log(BuildStateContext *ctx, const char *message) {
    if (ctx != NULL && ctx->log_message != NULL) {
        ctx->log_message(ctx->callback_ctx, message);
    }
}

/// @brief Logs a formatted message to the build state context.
/// @param ctx The build state context.
/// @param format The format string for the message.
/// @param ... The arguments for the format string.
static void build_state_logf(BuildStateContext *ctx, const char *format, ...) {
    char message[BUILD_STATE_LOG_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    build_state_log(ctx, message);
}

/// @brief Finds the original manifest task that produced a worker result.
/// @param tasks The array of tasks from the manifest.
/// @param task_count The number of tasks in the manifest.
/// @param source_path The source path from the worker result.
/// @param object_path The object path from the worker result.
/// @return The index of the matching task in the manifest, or -1 if no match is found.
static int find_original_task_index(
    const CompileTask *tasks,
    int task_count,
    const char *source_path,
    const char *object_path
) {
    if (source_path == NULL || object_path == NULL) {
        return -1;
    }

    // Try to find a task that matches both source and object paths.
    for (int i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].source_path, source_path) == 0 &&
            strcmp(tasks[i].object_path, object_path) == 0) {
            return i;
        }
    }

    // If no exact match, try to find a task that matches just the object path.
    for (int i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].object_path, object_path) == 0) {
            return i;
        }
    }

    return -1;
}

/// @brief Records the result of a compilation task for linking.
/// @param ctx The build state context.
/// @param source_path The source path from the worker result.
/// @param object_path The object path from the worker result.
/// @param success The success status of the task.
/// @return 1 if linking should be started, 0 otherwise.
int remocom_record_task_result_for_link(
    BuildStateContext *ctx,
    const char *source_path,
    const char *object_path,
    int success
) {
    int should_link = 0;
    int task_index = find_original_task_index(
        ctx->tasks, ctx->task_count,
        source_path, object_path
    );

    pthread_mutex_lock(ctx->build_mutex);

    if (task_index < 0) {
        *ctx->build_failed = 1;
        build_state_logf(
            ctx,
            "[ERROR] Task result did not match manifest | source=%s | object=%s\n",
            source_path != NULL ? source_path : "<unknown>",
            object_path != NULL ? object_path : "<unknown>"
        );
        pthread_mutex_unlock(ctx->build_mutex);
        return 0;
    }

    // If the task succeeded, mark the object as ready.
    // If it failed and wasn't already marked ready, mark the build as failed.
    if (success) {
        if (!ctx->object_ready[task_index]) {
            ctx->object_ready[task_index] = 1;
            (*ctx->object_ready_count)++;
            build_state_logf(ctx, "OBJECT READY | source=%s | object=%s | completed=%d/%d\n",
                ctx->tasks[task_index].source_path, ctx->tasks[task_index].object_path,
                *ctx->object_ready_count, ctx->task_count);
        } else {
            build_state_logf(ctx, "DUPLICATE OBJECT RESULT IGNORED | source=%s | object=%s\n",
                source_path, object_path);
        }
    } else if (!ctx->object_ready[task_index]) {
        *ctx->build_failed = 1;
        build_state_logf(
            ctx,
            "[ERROR] Build task failed; linking skipped | source=%s | object=%s\n",
            ctx->tasks[task_index].source_path,
            ctx->tasks[task_index].object_path
        );
    }

    // If all tasks are ready and we haven't already started linking or failed,
    // indicate that linking should start.
    if (!*ctx->build_failed && !*ctx->link_started && *ctx->object_ready_count == ctx->task_count) {
        *ctx->link_started = 1;
        should_link = 1;
    }

    pthread_mutex_unlock(ctx->build_mutex);
    return should_link;
}
