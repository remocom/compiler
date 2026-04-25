#include "linker.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../common/common.h"

#define LINK_OUTPUT_SIZE 65536
#define LINKER_LOG_PATH "linker.log"
#define LINKER_LOG_MESSAGE_SIZE (LINK_OUTPUT_SIZE + 512)

/// @brief Writes a formatted message to the linker log if it is available.
/// @param ctx The linker context.
/// @param message The message to log.
static void linker_log(const LinkerContext *ctx, const char *message) {
    if (ctx != NULL && ctx->log_message != NULL) {
        ctx->log_message(ctx->callback_ctx, message);
    }
}

/// @brief Writes a formatted message to the linker log if it is available.
/// @param ctx The linker context.
/// @param format The format string for the message.
/// @param ... The arguments for the format string.
static void linker_logf(const LinkerContext *ctx, const char *format, ...) {
    char message[LINKER_LOG_MESSAGE_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    linker_log(ctx, message);
}

/// @brief Writes a formatted message to the linker log if it is available.
/// @param linker_log The file pointer for the linker log.
/// @param format The format string for the message.
/// @param ... The arguments for the format string.
static void write_linker_log(FILE *linker_log, const char *format, ...) {
    if (linker_log == NULL) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(linker_log, format, args);
    va_end(args);
    fflush(linker_log);
}

/// @brief Selects the linker driver for the manifest's object files.
/// @param ctx The linker context containing the manifest and tasks.
/// @return The name of the linker driver to use.
static const char *select_linker_driver(const LinkerContext *ctx) {
    for (int i = 0; i < ctx->task_count; i++) {
        if (strcmp(remocom_select_source_driver(ctx->tasks[i].source_path), "g++") == 0) {
            return "g++";
        }
    }

    return "gcc";
}

/// @brief Removes intermediate object files after a successful link unless retention was requested.
/// @param ctx The linker context containing object file paths and retention setting.
static void cleanup_object_files(const LinkerContext *ctx) {
    if (ctx->keep_objects) {
        linker_logf(ctx, "OBJECT CLEANUP SKIPPED | keep_objects=true\n");
        return;
    }

    for (int i = 0; i < ctx->task_count; i++) {
        const char *object_path = ctx->tasks[i].object_path;
        if (unlink(object_path) == 0) {
            linker_logf(ctx, "OBJECT REMOVED | object=%s\n", object_path);
            printf("Removed object file: %s\n", object_path);
        } else if (errno != ENOENT) {
            linker_logf(ctx, "[WARNING] Failed to remove object file | object=%s | error=%s\n",
                object_path, strerror(errno));
            printf("Failed to remove object file %s: %s\n", object_path, strerror(errno));
        }
    }
}

int remocom_run_link_step(const LinkerContext *ctx) {
    const BuildManifest *manifest = ctx->manifest;
    const char *linker_driver = select_linker_driver(ctx);
    FILE *linker_log = fopen(LINKER_LOG_PATH, "w");
    if (linker_log == NULL) {
        linker_logf(ctx, "[ERROR] Failed to create linker log %s: %s\n", LINKER_LOG_PATH, strerror(errno));
        printf("Failed to create linker log %s: %s\n", LINKER_LOG_PATH, strerror(errno));
    } else {
        linker_logf(ctx, "LINKER LOG PRODUCED | path=%s\n", LINKER_LOG_PATH);
    }

    write_linker_log(linker_log, "Remocom linker log\n");
    write_linker_log(linker_log, "output=%s\n", manifest->output);
    write_linker_log(linker_log, "objects=%d\n", ctx->task_count);
    write_linker_log(linker_log, "driver=%s\n", linker_driver);
    write_linker_log(linker_log, "command=");
    write_linker_log(linker_log, "%s", linker_driver);
    for (int i = 0; i < ctx->task_count; i++) {
        write_linker_log(linker_log, " %s", ctx->tasks[i].object_path);
    }
    for (int i = 0; i < manifest->flag_count; i++) {
        write_linker_log(linker_log, " %s", manifest->flags[i]);
    }
    write_linker_log(linker_log, " -o %s\n\n", manifest->output);

    linker_logf(ctx, "LINK STARTED | output=%s | objects=%d\n", manifest->output, ctx->task_count);
    write_linker_log(linker_log, "status=started\n");
    printf("Linking %d object files into %s\n", ctx->task_count, manifest->output);

    char *argv[MAX_TASKS + MAX_FLAGS + 4];
    int argc = 0;
    argv[argc++] = (char *)linker_driver;
    for (int i = 0; i < ctx->task_count; i++) {
        argv[argc++] = (char *)ctx->tasks[i].object_path;
    }
    for (int i = 0; i < manifest->flag_count; i++) {
        argv[argc++] = (char *)manifest->flags[i];
    }
    argv[argc++] = "-o";
    argv[argc++] = (char *)manifest->output;
    argv[argc] = NULL;

    char output[LINK_OUTPUT_SIZE] = {0};
    int status = 0;
    if (!remocom_run_process_capture(argv, output, sizeof(output), &status)) {
        linker_logf(ctx, "[ERROR] Link process failed while producing %s\n", manifest->output);
        write_linker_log(linker_log, "status=failed\n");
        write_linker_log(linker_log, "error=link process failed");
        if (errno != 0) {
            write_linker_log(linker_log, ": %s", strerror(errno));
        }
        write_linker_log(linker_log, "\n");
        printf("Link failed for %s\n", manifest->output);
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 0;
    }

    if (output[0] != '\0') {
        linker_logf(ctx, "\n\n--- linker output (%s) ---\n%s\n--- end linker output ---\n\n",
            manifest->output, output);
        write_linker_log(linker_log, "--- linker output ---\n%s\n--- end linker output ---\n", output);
        printf("\n\n--- linker output (%s) ---\n%s--- end linker output ---\n\n",
            manifest->output, output);
    } else {
        write_linker_log(linker_log, "--- linker output ---\n<empty>\n--- end linker output ---\n");
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        linker_logf(ctx, "LINK SUCCEEDED | output=%s\n", manifest->output);
        write_linker_log(linker_log, "status=succeeded\nexit_code=0\n");
        printf("Link succeeded: %s\n", manifest->output);
        cleanup_object_files(ctx);
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 1;
    }

    if (WIFEXITED(status)) {
        linker_logf(ctx, "[ERROR] LINK FAILED | output=%s | exit_code=%d\n",
            manifest->output, WEXITSTATUS(status));
        write_linker_log(linker_log, "status=failed\nexit_code=%d\n", WEXITSTATUS(status));
        printf("Link failed for %s with exit code %d\n",
            manifest->output, WEXITSTATUS(status));
    } else {
        linker_logf(ctx, "[ERROR] LINK FAILED | output=%s | abnormal termination\n", manifest->output);
        write_linker_log(linker_log, "status=failed\ntermination=abnormal\n");
        printf("Link failed for %s\n", manifest->output);
    }

    if (linker_log != NULL) {
        fclose(linker_log);
    }
    return 0;
}
