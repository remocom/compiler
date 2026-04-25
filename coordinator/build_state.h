#ifndef REMOCOM_BUILD_STATE_H
#define REMOCOM_BUILD_STATE_H

#include <pthread.h>

#include "coordinator_types.h"

/// @brief Context for tracking build state and determining when linking can start.
typedef void (*BuildStateLogFn)(void *callback_ctx, const char *message);

/// @brief Dependencies and counters used to track build completion state.
typedef struct {
    const CompileTask *tasks;
    int task_count;
    int *object_ready;
    int *object_ready_count;
    int *build_failed;
    int *link_started;
    pthread_mutex_t *build_mutex;
    BuildStateLogFn log_message;
    void *callback_ctx;
} BuildStateContext;

/// @brief Records one task result and determines whether linking should start.
/// @return 1 if all required object files are ready and linking should start, 0 otherwise.
int remocom_record_task_result_for_link(
    BuildStateContext *ctx,
    const char *source_path,
    const char *object_path,
    int success
);

#endif
