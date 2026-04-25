#ifndef REMOCOM_TASK_DISPATCH_H
#define REMOCOM_TASK_DISPATCH_H

#include <pthread.h>

#include "coordinator_types.h"
#include "manifest_loader.h"

typedef void (*TaskDispatchLogFn)(void *callback_ctx, const char *message);
typedef void (*TaskDispatchMarkActiveFn)(void *callback_ctx, int client_fd, const CompileTask *task);

/// @brief Dependencies and callbacks needed to assign compile tasks to workers.
typedef struct {
    CompileTask *task_queue;
    int max_tasks;
    int *total_tasks;
    int *next_task_index;
    const BuildManifest *manifest;
    pthread_mutex_t *task_mutex;
    TaskDispatchLogFn log_message;
    TaskDispatchMarkActiveFn mark_task_active;
    void *callback_ctx;
} TaskDispatchContext;

/// @brief Appends a task to the queue so it can be assigned again.
/// @param ctx The task dispatch context.
/// @param task The compile task to re-enqueue for assignment.
void remocom_enqueue_task_for_reassign(TaskDispatchContext *ctx, const CompileTask *task);

/// @brief Assigns the next queued task to a worker or reports no-task.
/// @param ctx The task dispatch context containing the task queue and synchronization primitives.
/// @param client_fd The file descriptor of the worker to assign the task to.
/// @param node_id The ID of the worker node to assign the task to.
void remocom_assign_task_to_worker(TaskDispatchContext *ctx, int client_fd, int node_id);

#endif
