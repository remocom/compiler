#ifndef REMOCOM_LINKER_H
#define REMOCOM_LINKER_H

#include "coordinator_types.h"
#include "manifest_loader.h"

/// @brief Function pointer type for logging messages from the linker context.
typedef void (*LinkerLogFn)(void *callback_ctx, const char *message);

/// @brief Dependencies and callbacks needed to run the final link step.
typedef struct {
    const BuildManifest *manifest;
    const CompileTask *tasks;
    int task_count;
    LinkerLogFn log_message;
    void *callback_ctx;
} LinkerContext;

/// @brief Links all manifest object files into the requested build output.
/// @return 1 on successful link, 0 otherwise.
int remocom_run_link_step(const LinkerContext *ctx);

#endif
