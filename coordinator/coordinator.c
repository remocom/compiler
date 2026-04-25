#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions
#include <pthread.h>    // thread library for handling multiple workers
#include <cjson/cJSON.h> // Ultra lightweight JSON parser library
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/wait.h>
#include "../common/common.h"
#include "manifest_loader.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PORT 5000
#define BUFFER_SIZE 4096
#define MAX_WORKERS 100
#define MAX_TASKS REMOCOM_MAX_SOURCES
#define MAX_MANIFEST_VALUE REMOCOM_MAX_MANIFEST_VALUE
#define MAX_FLAGS REMOCOM_MAX_FLAGS
#define MAX_TRANSFER_FILES (REMOCOM_MAX_HEADERS + 1)
#define DEPENDENCY_OUTPUT_SIZE 65536
#define LINK_OUTPUT_SIZE 65536
#define LINKER_LOG_PATH "linker.log"

/// @brief Represents a compile task derived from the manifest, containing source/object paths, build output, and flags.
typedef struct {
    char source_path[MAX_MANIFEST_VALUE];
    char object_path[MAX_MANIFEST_VALUE];
    char build_output[MAX_MANIFEST_VALUE];
    char flags[MAX_FLAGS][MAX_MANIFEST_VALUE];
    int flag_count;
} CompileTask;

/// @brief Represents a file that needs to be transferred to a worker, including its path,
/// kind (source or header), and size in bytes.
typedef struct {
    const char *path;
    const char *kind;
    uint64_t size;
} TransferFile;

/// @brief Adds a file to the list of files to be transferred.
/// @param transfer_files Array of TransferFile structs being built for the current task assignment.
/// @param transfer_paths Array of strings containing the paths of the files to be transferred.
/// @param transfer_file_count Pointer to an integer representing the number of files in the transfer list.
/// @param path The path of the file to be added.
/// @param kind The kind of the file (source or header).
/// @return 1 if successful, 0 otherwise.
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

/// @brief Collects the source dependencies for a given compile task.
/// @param task The compile task for which to collect dependencies.
/// @param transfer_files Array of TransferFile structs being built for the current task assignment.
/// @param transfer_paths Array of strings containing the paths of the files to be transferred.
/// @param transfer_file_count Pointer to an integer representing the number of files
/// in the transfer list.
/// @param error_buf A buffer to store any error messages.
/// @param error_buf_size The size of the error buffer.
/// @return 1 if successful, 0 otherwise.
static int collect_source_dependencies(
    const CompileTask *task,
    TransferFile *transfer_files,
    char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE],
    int *transfer_file_count,
    char *error_buf,
    size_t error_buf_size
) {
    const char *compiler_driver = remocom_select_source_driver(task->source_path);
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(error_buf, error_buf_size, "pipe() failed while scanning dependencies");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(error_buf, error_buf_size, "fork() failed while scanning dependencies");
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            close(pipefd[1]);
            _exit(127);
        }
        close(pipefd[1]);

        char *argv[MAX_FLAGS + 4];
        int argc = 0;
        argv[argc++] = (char *)compiler_driver;
        for (int i = 0; i < task->flag_count; i++) {
            argv[argc++] = (char *)task->flags[i];
        }
        argv[argc++] = "-MM";
        argv[argc++] = (char *)task->source_path;
        argv[argc] = NULL;

        execvp(compiler_driver, argv);
        _exit(127);
    }

    close(pipefd[1]);

    char output[DEPENDENCY_OUTPUT_SIZE];
    int read_ok = remocom_read_process_output(pipefd[0], output, sizeof(output));
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !read_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
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
            const char *kind = strcmp(token, task->source_path) == 0 ? "source" : "header";
            if (!add_transfer_file(transfer_files, transfer_paths, transfer_file_count, token, kind)) {
                snprintf(error_buf, error_buf_size, "Too many dependency files for %s", task->source_path);
                return 0;
            }
        }
        token = strtok(NULL, " ");
    }

    return 1;
}

/// @brief Represents the status of a worker node, which can be either dead or alive.
typedef enum { dead, alive } NodeStatus;

/// @brief Represents a worker node in the system, containing its ID, socket, IP address, last heartbeat timestamp, and status (alive or dead).
typedef struct {
    int nodeID;
    int socketID;
    char ip_address[INET_ADDRSTRLEN];
    time_t last_heartbeat; // timestamp
    NodeStatus status; // alive or dead
    int handshake_completed;
    int has_active_task; // 1 if worker is currently assigned a task
    CompileTask current_task; // Copy of the task (valid only when has_active_task == 1)
} Node;


static Node workers[MAX_WORKERS];
static int worker_count = 0;
static FILE *log_file;

static CompileTask task_queue[MAX_TASKS];
static int total_tasks = 0;
static int original_task_count = 0;
static int next_task_index = 0;
static BuildManifest build_manifest;
static int object_ready[MAX_TASKS];
static int object_ready_count = 0;
static int build_failed = 0;
static int link_started = 0;

static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t build_mutex = PTHREAD_MUTEX_INITIALIZER;

static void send_json_message(int client_fd, const char *type_str, const char *payload_str);
static void *handle_worker(void *arg);
static void write_linker_log(FILE *linker_log, const char *format, ...);

static int change_to_manifest_directory(const char *manifest_path, char *error_buf, size_t error_buf_size) {
    char manifest_copy[PATH_MAX];
    snprintf(manifest_copy, sizeof(manifest_copy), "%s", manifest_path);

    char *last_slash = strrchr(manifest_copy, '/');
    if (last_slash == NULL) {
        return 1;
    }

    if (last_slash == manifest_copy) {
        last_slash[1] = '\0';
    } else {
        *last_slash = '\0';
    }

    if (chdir(manifest_copy) != 0) {
        snprintf(error_buf, error_buf_size, "Failed to enter manifest directory '%s': %s",
            manifest_copy, strerror(errno));
        return 0;
    }

    return 1;
}

/// @brief Builds a default object-file path from a source path.
/// @param source_path Source path from manifest.
/// @param object_path Destination for derived object path.
/// @param object_path_size Destination size.
/// @return 1 on success, 0 if output buffer is too small.
static int derive_object_path(const char *source_path, char *object_path, size_t object_path_size) {
    char temp[MAX_MANIFEST_VALUE];
    snprintf(temp, sizeof(temp), "%s", source_path);

    const char *last_slash = strrchr(temp, '/');
    char *last_dot = strrchr(temp, '.');
    if (last_dot != NULL && (last_slash == NULL || last_dot > last_slash)) {
        *last_dot = '\0';
    }

    int needed = snprintf(object_path, object_path_size, "%s.o", temp);
    return needed > 0 && (size_t)needed < object_path_size;
}

/// @brief Selects the linker driver for the manifest's object files.
/// @return "g++" when any original source is C++, otherwise "gcc".
static const char *select_linker_driver(void) {
    for (int i = 0; i < original_task_count; i++) {
        if (strcmp(remocom_select_source_driver(task_queue[i].source_path), "g++") == 0) {
            return "g++";
        }
    }

    return "gcc";
}

/// @brief Loads a subset of TOML manifest format for the [build] section.
/// @param manifest_path Path to manifest file.
/// @param manifest Parsed output manifest.
/// @param error_buf Destination buffer for validation errors.
/// @param error_buf_size Size of error buffer.
/// @return 1 on success, 0 on parse/validation failure.
static int load_manifest_file(const char *manifest_path, BuildManifest *manifest, char *error_buf, size_t error_buf_size) {
    return remocom_load_manifest_file(manifest_path, manifest, error_buf, error_buf_size);
}

/// @brief Converts manifest sources into the coordinator's task queue.
/// @param manifest Parsed build manifest.
/// @param error_buf Destination buffer for validation errors.
/// @param error_buf_size Size of error buffer.
/// @return 1 on success, 0 when queue/object path validation fails.
static int build_compile_tasks_from_manifest(const BuildManifest *manifest, char *error_buf, size_t error_buf_size) {
    total_tasks = 0;
    original_task_count = 0;
    next_task_index = 0;
    object_ready_count = 0;
    build_failed = 0;
    link_started = 0;
    memset(object_ready, 0, sizeof(object_ready));

    for (int i = 0; i < manifest->source_count; i++) {
        if (total_tasks >= MAX_TASKS) {
            snprintf(error_buf, error_buf_size, "Manifest contains more than %d sources", MAX_TASKS);
            return 0;
        }

        CompileTask *task = &task_queue[total_tasks];
        snprintf(task->source_path, sizeof(task->source_path), "%s", manifest->sources[i]);
        if (!derive_object_path(task->source_path, task->object_path, sizeof(task->object_path))) {
            snprintf(error_buf, error_buf_size, "Object path too long for source: %s", task->source_path);
            return 0;
        }

        snprintf(task->build_output, sizeof(task->build_output), "%s", manifest->output);
        task->flag_count = manifest->flag_count;
        for (int flag_index = 0; flag_index < manifest->flag_count; flag_index++) {
            snprintf(task->flags[flag_index], sizeof(task->flags[flag_index]), "%s", manifest->flags[flag_index]);
        }

        total_tasks++;
    }

    original_task_count = total_tasks;
    return 1;
}

/// @brief Prints coordinator usage help.
/// @param program_name argv[0] executable name.
static void print_usage(const char *program_name) {
    printf("Usage: %s --manifest <manifest.toml>\n", program_name);
    printf("   or: %s -m <manifest.toml>\n", program_name);
}

/// @brief Parses coordinator CLI args to find manifest path.
/// @param argc Argument count.
/// @param argv Argument list.
/// @param manifest_path Receives parsed manifest path.
/// @return 1 when CLI args are valid and manifest is provided, 0 otherwise.
static int parse_manifest_cli_flag(int argc, char **argv, const char **manifest_path) {
    *manifest_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manifest") == 0 || strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }

            *manifest_path = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return 0;
        }
    }

    return *manifest_path != NULL;
}

/// @brief Validates and applies handshake payload for a worker.
/// @param worker Worker record to update if handshake is accepted.
/// @param payload Parsed payload object from incoming message.
/// @param reason_buf Output buffer for mismatch reason.
/// @param reason_buf_len Size of reason buffer.
/// @return 1 if payload matches coordinator requirements, 0 otherwise.
static int validate_handshake_payload(Node *worker, cJSON *payload, char *reason_buf, size_t reason_buf_len) {
    if (payload == NULL || !cJSON_IsObject(payload)) {
        snprintf(reason_buf, reason_buf_len, "Handshake payload must be an object");
        return 0;
    }

    cJSON *gcc_version = cJSON_GetObjectItem(payload, HANDSHAKE_KEY_GCC_VERSION);
    cJSON *target_arch = cJSON_GetObjectItem(payload, HANDSHAKE_KEY_TARGET_ARCH);
    cJSON *target_os = cJSON_GetObjectItem(payload, HANDSHAKE_KEY_TARGET_OS);
    cJSON *rpc_protocol_version = cJSON_GetObjectItem(payload, HANDSHAKE_KEY_RPC_PROTOCOL_VERSION);

    if (!cJSON_IsString(gcc_version) || !cJSON_IsString(target_arch) ||
        !cJSON_IsString(target_os) || !cJSON_IsNumber(rpc_protocol_version)) {
        snprintf(reason_buf, reason_buf_len, "Handshake payload missing required fields");
        return 0;
    }

    if (strcmp(gcc_version->valuestring, __VERSION__) != 0) {
        snprintf(reason_buf, reason_buf_len, "GCC mismatch worker=%s coordinator=%s",
            gcc_version->valuestring, __VERSION__);
        return 0;
    }

    const char *expected_arch = remocom_detect_target_arch();
    if (strcmp(target_arch->valuestring, expected_arch) != 0) {
        snprintf(reason_buf, reason_buf_len, "Architecture mismatch worker=%s coordinator=%s",
            target_arch->valuestring, expected_arch);
        return 0;
    }

    const char *expected_os = remocom_detect_target_os();
    if (strcmp(target_os->valuestring, expected_os) != 0) {
        snprintf(reason_buf, reason_buf_len, "OS mismatch worker=%s coordinator=%s",
            target_os->valuestring, expected_os);
        return 0;
    }

    int worker_protocol_version = rpc_protocol_version->valueint;
    if (worker_protocol_version != REMOCOM_RPC_PROTOCOL_VERSION) {
        snprintf(reason_buf, reason_buf_len, "RPC protocol version mismatch worker=%d coordinator=%d",
            worker_protocol_version, REMOCOM_RPC_PROTOCOL_VERSION);
        return 0;
    }

    worker->handshake_completed = 1;
    return 1;
}

/// @brief Thread-safe logging helper for coordinator events.
/// @param format printf-style format string.
static void log_event(const char *format, ...) {
    pthread_mutex_lock(&log_mutex);

    // Use vfprintf to handle the variable argument list for formatted logging.
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

/// @brief Writes a formatted message to the linker log if it is available.
/// @param linker_log Linker log file handle.
/// @param format printf-style format string.
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

/// @brief Finds the original manifest task that produced a worker result.
/// @param source_path Source path reported by the worker.
/// @param object_path Object path reported by the worker.
/// @return Original task index, or -1 if the result does not match the manifest.
static int find_original_task_index(const char *source_path, const char *object_path) {
    if (source_path == NULL || object_path == NULL) {
        return -1;
    }

    for (int i = 0; i < original_task_count; i++) {
        if (strcmp(task_queue[i].source_path, source_path) == 0 &&
            strcmp(task_queue[i].object_path, object_path) == 0) {
            return i;
        }
    }

    for (int i = 0; i < original_task_count; i++) {
        if (strcmp(task_queue[i].object_path, object_path) == 0) {
            return i;
        }
    }

    return -1;
}

/// @brief Records one task result and determines whether all objects are ready for linking.
/// @param source_path Source path reported by the worker.
/// @param object_path Object path reported by the worker.
/// @param success 1 when the object was compiled and received successfully.
/// @return 1 if the coordinator should start linking now, 0 otherwise.
static int record_task_result_for_link(const char *source_path, const char *object_path, int success) {
    int should_link = 0;
    int task_index = find_original_task_index(source_path, object_path);

    pthread_mutex_lock(&build_mutex);

    if (task_index < 0) {
        build_failed = 1;
        log_event("[ERROR] Task result did not match manifest | source=%s | object=%s\n",
            source_path != NULL ? source_path : "<unknown>",
            object_path != NULL ? object_path : "<unknown>");
        pthread_mutex_unlock(&build_mutex);
        return 0;
    }

    if (success) {
        if (!object_ready[task_index]) {
            object_ready[task_index] = 1;
            object_ready_count++;
            log_event("OBJECT READY | source=%s | object=%s | completed=%d/%d\n",
                task_queue[task_index].source_path, task_queue[task_index].object_path,
                object_ready_count, original_task_count);
        } else {
            log_event("DUPLICATE OBJECT RESULT IGNORED | source=%s | object=%s\n",
                source_path, object_path);
        }
    } else if (!object_ready[task_index]) {
        build_failed = 1;
        log_event("[ERROR] Build task failed; linking skipped | source=%s | object=%s\n",
            task_queue[task_index].source_path, task_queue[task_index].object_path);
    }

    if (!build_failed && !link_started && object_ready_count == original_task_count) {
        link_started = 1;
        should_link = 1;
    }

    pthread_mutex_unlock(&build_mutex);
    return should_link;
}

/// @brief Links all received manifest object files into the requested build output.
/// @return 1 on successful link, 0 otherwise.
static int run_link_step(void) {
    const char *linker_driver = select_linker_driver();
    FILE *linker_log = fopen(LINKER_LOG_PATH, "w");
    if (linker_log == NULL) {
        log_event("[ERROR] Failed to create linker log %s: %s\n", LINKER_LOG_PATH, strerror(errno));
        printf("Failed to create linker log %s: %s\n", LINKER_LOG_PATH, strerror(errno));
    } else {
        log_event("LINKER LOG PRODUCED | path=%s\n", LINKER_LOG_PATH);
    }

    write_linker_log(linker_log, "Remocom linker log\n");
    write_linker_log(linker_log, "output=%s\n", build_manifest.output);
    write_linker_log(linker_log, "objects=%d\n", original_task_count);
    write_linker_log(linker_log, "driver=%s\n", linker_driver);
    write_linker_log(linker_log, "command=");
    write_linker_log(linker_log, "%s", linker_driver);
    for (int i = 0; i < original_task_count; i++) {
        write_linker_log(linker_log, " %s", task_queue[i].object_path);
    }
    for (int i = 0; i < build_manifest.flag_count; i++) {
        write_linker_log(linker_log, " %s", build_manifest.flags[i]);
    }
    write_linker_log(linker_log, " -o %s\n\n", build_manifest.output);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        log_event("[ERROR] pipe() failed while linking output %s\n", build_manifest.output);
        write_linker_log(linker_log, "error=pipe() failed: %s\n", strerror(errno));
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 0;
    }

    log_event("LINK STARTED | output=%s | objects=%d\n", build_manifest.output, original_task_count);
    write_linker_log(linker_log, "status=started\n");
    printf("Linking %d object files into %s\n", original_task_count, build_manifest.output);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        log_event("[ERROR] fork() failed while linking output %s\n", build_manifest.output);
        write_linker_log(linker_log, "error=fork() failed: %s\n", strerror(errno));
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) {
            close(pipefd[1]);
            _exit(127);
        }
        close(pipefd[1]);

        char *argv[MAX_TASKS + MAX_FLAGS + 4];
        int argc = 0;
        argv[argc++] = (char *)linker_driver;
        for (int i = 0; i < original_task_count; i++) {
            argv[argc++] = task_queue[i].object_path;
        }
        for (int i = 0; i < build_manifest.flag_count; i++) {
            argv[argc++] = build_manifest.flags[i];
        }
        argv[argc++] = "-o";
        argv[argc++] = build_manifest.output;
        argv[argc] = NULL;

        execvp(linker_driver, argv);
        _exit(127);
    }

    close(pipefd[1]);

    char output[LINK_OUTPUT_SIZE];
    int read_ok = remocom_read_process_output(pipefd[0], output, sizeof(output));
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !read_ok) {
        log_event("[ERROR] Link process failed while producing %s\n", build_manifest.output);
        write_linker_log(linker_log, "status=failed\n");
        write_linker_log(linker_log, "error=link process failed");
        if (errno != 0) {
            write_linker_log(linker_log, ": %s", strerror(errno));
        }
        write_linker_log(linker_log, "\n");
        printf("Link failed for %s\n", build_manifest.output);
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 0;
    }

    if (output[0] != '\0') {
        log_event("\n\n--- linker output (%s) ---\n%s\n--- end linker output ---\n\n",
            build_manifest.output, output);
        write_linker_log(linker_log, "--- linker output ---\n%s\n--- end linker output ---\n", output);
        printf("\n\n--- linker output (%s) ---\n%s--- end linker output ---\n\n",
            build_manifest.output, output);
    } else {
        write_linker_log(linker_log, "--- linker output ---\n<empty>\n--- end linker output ---\n");
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        log_event("LINK SUCCEEDED | output=%s\n", build_manifest.output);
        write_linker_log(linker_log, "status=succeeded\nexit_code=0\n");
        printf("Link succeeded: %s\n", build_manifest.output);
        if (linker_log != NULL) {
            fclose(linker_log);
        }
        return 1;
    }

    if (WIFEXITED(status)) {
        log_event("[ERROR] LINK FAILED | output=%s | exit_code=%d\n",
            build_manifest.output, WEXITSTATUS(status));
        write_linker_log(linker_log, "status=failed\nexit_code=%d\n", WEXITSTATUS(status));
        printf("Link failed for %s with exit code %d\n",
            build_manifest.output, WEXITSTATUS(status));
    } else {
        log_event("[ERROR] LINK FAILED | output=%s | abnormal termination\n", build_manifest.output);
        write_linker_log(linker_log, "status=failed\ntermination=abnormal\n");
        printf("Link failed for %s\n", build_manifest.output);
    }

    if (linker_log != NULL) {
        fclose(linker_log);
    }
    return 0;
}

/// @brief Finds a worker by socket while workers_mutex is held.
/// @param client_fd Socket descriptor to locate.
/// @return Pointer to worker entry or NULL if not found.
static Node *find_worker_by_socket_unlocked(int client_fd) {
    for (int i = 0; i < worker_count; i++) {
        if (workers[i].socketID == client_fd) {
            return &workers[i];
        }
    }
    return NULL;
}

/// @brief Appends a task to the end of the main queue for reassignment. Called must hold task_mutex
/// @param task Tasks to re-enqueue
static void enqueue_for_reassign_unlocked(const CompileTask *task){
    if (total_tasks < MAX_TASKS){
        task_queue[total_tasks] = *task;
        total_tasks++;
    } else {
        log_event("[ERROR] Cannot re-enqueue task (queue full, MAX_TASKS = %d): source=%s - build will be incomplete\n",
            MAX_TASKS, task->source_path);
    }
}

static void requeue_task_for_prepare_failure(const CompileTask *task) {
    pthread_mutex_lock(&task_mutex);
    enqueue_for_reassign_unlocked(task);
    pthread_mutex_unlock(&task_mutex);
}

/// @brief Assigns the next queued task to a worker or reports no-task.
/// @param client_fd Worker socket.
/// @param node_id Worker node ID for logging.
static void assign_task_to_worker(int client_fd, int node_id) {
    // Lock task queue so only one worker gets each task.
    pthread_mutex_lock(&task_mutex);

    // Check if there are tasks left to assign.
    if (next_task_index < total_tasks) {
        CompileTask task = task_queue[next_task_index];
        next_task_index++;
        pthread_mutex_unlock(&task_mutex);

        TransferFile transfer_files[MAX_TRANSFER_FILES];
        char transfer_paths[MAX_TRANSFER_FILES][MAX_MANIFEST_VALUE];
        int transfer_file_count = 0;
        char dependency_error[1024];

        if (!add_transfer_file(transfer_files, transfer_paths, &transfer_file_count, task.source_path, "source")) {
            log_event("[ERROR] Unable to queue source transfer | Node %d | Source: %s\n",
                node_id, task.source_path);
            requeue_task_for_prepare_failure(&task);
            send_json_message(client_fd, "task_error", "Unable to queue source transfer");
            return;
        }

        if (!collect_source_dependencies(&task, transfer_files, transfer_paths, &transfer_file_count,
            dependency_error, sizeof(dependency_error))) {
            log_event("[ERROR] %s\n", dependency_error);
            requeue_task_for_prepare_failure(&task);
            send_json_message(client_fd, "task_error", dependency_error);
            return;
        }

        for (int i = 0; i < build_manifest.header_count; i++) {
            if (!add_transfer_file(transfer_files, transfer_paths, &transfer_file_count,
                build_manifest.headers[i], "header")) {
                log_event("[ERROR] Too many transfer files for task | Node %d | Source: %s\n",
                    node_id, task.source_path);
                requeue_task_for_prepare_failure(&task);
                send_json_message(client_fd, "task_error", "Too many transfer files");
                return;
            }
        }

        for (int i = 0; i < transfer_file_count; i++) {
            if (!remocom_get_file_size(transfer_files[i].path, &transfer_files[i].size)) {
                log_event("[ERROR] Transfer file unavailable for task | Node %d | File: %s\n",
                    node_id, transfer_files[i].path);
                requeue_task_for_prepare_failure(&task);
                send_json_message(client_fd, "task_error", "Transfer file unavailable");
                return;
            }
        }

        // Record ownership so we can re-enqueue if this worker dies
        pthread_mutex_lock(&workers_mutex);
        Node *worker = find_worker_by_socket_unlocked(client_fd);
        if(worker != NULL){
            worker->current_task = task;
            worker->has_active_task = 1;
        }
        pthread_mutex_unlock(&workers_mutex);

        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "source", task.source_path);
        cJSON_AddStringToObject(payload, "object", task.object_path);
        cJSON_AddStringToObject(payload, "output", task.build_output);

        cJSON *files = cJSON_AddArrayToObject(payload, "files");
        for (int i = 0; i < transfer_file_count; i++) {
            char size_text[32];
            snprintf(size_text, sizeof(size_text), "%" PRIu64, transfer_files[i].size);

            cJSON *file_payload = cJSON_CreateObject();
            cJSON_AddStringToObject(file_payload, "path", transfer_files[i].path);
            cJSON_AddStringToObject(file_payload, "kind", transfer_files[i].kind);
            cJSON_AddStringToObject(file_payload, "size", size_text);
            cJSON_AddItemToArray(files, file_payload);
        }

        cJSON *flags = cJSON_AddArrayToObject(payload, "flags");
        for (int i = 0; i < task.flag_count; i++) {
            cJSON_AddItemToArray(flags, cJSON_CreateString(task.flags[i]));
        }

        int sent_ok = remocom_send_json_with_payload(client_fd, MSG_TYPE_TASK_ASSIGNMENT, payload);
        for (int i = 0; sent_ok && i < transfer_file_count; i++) {
            sent_ok = remocom_send_file_stream(client_fd, transfer_files[i].path);
        }

        if (!sent_ok) {
            log_event("[ERROR] Failed sending source file to Node %d | Source: %s\n",
                node_id, task.source_path);
        }
        log_event("TASK ASSIGNED | Node %d | Source: %s | Object: %s\n",
            node_id, task.source_path, task.object_path);
        return;
    }

    pthread_mutex_unlock(&task_mutex);
    send_json_message(client_fd, MSG_TYPE_NO_TASK, "No tasks available");
}

/// @brief Logs compile task results reported by workers.
/// @param node_id Worker node ID.
/// @param payload Parsed task result payload.
/// @return 1 if all object files are ready and the caller should start linking.
static int handle_task_result(int client_fd, int node_id, cJSON *payload) {
    if (!cJSON_IsObject(payload)) {
        log_event("TASK RESULT | Node %d | Invalid payload\n", node_id);
        return 0;
    }

    // Extract fields from payload with validation.
    // If any field is missing or of the wrong type, log the issue but attempt to print whatever information is available.
    cJSON *source = cJSON_GetObjectItem(payload, "source");
    cJSON *object = cJSON_GetObjectItem(payload, "object");
    cJSON *status = cJSON_GetObjectItem(payload, "status");
    cJSON *exit_code = cJSON_GetObjectItem(payload, "exit_code");
    cJSON *message = cJSON_GetObjectItem(payload, "message");
    cJSON *output = cJSON_GetObjectItem(payload, "compiler_output");
    cJSON *has_object = cJSON_GetObjectItem(payload, "has_object");
    cJSON *object_size = cJSON_GetObjectItem(payload, "object_size");

    const char *source_str = cJSON_IsString(source) ? source->valuestring : "<unknown>";
    const char *object_str = cJSON_IsString(object) ? object->valuestring : "<unknown>";
    const char *status_str = cJSON_IsString(status) ? status->valuestring : "<unknown>";
    int exit_code_val = cJSON_IsNumber(exit_code) ? exit_code->valueint : -1;
    const char *message_str = cJSON_IsString(message) ? message->valuestring : "<none>";
    int should_receive_object = cJSON_IsBool(has_object) && cJSON_IsTrue(has_object) && cJSON_IsString(object);
    int compile_succeeded = strcmp(status_str, "success") == 0 && exit_code_val == 0;
    int object_received = 0;
    uint64_t object_size_val = 0;
    int has_object_size = remocom_parse_u64_string(object_size, &object_size_val);

    log_event(
        "TASK RESULT | Node %d | source=%s | object=%s | status=%s | exit_code=%d | message=%s\n",
        node_id, source_str, object_str, status_str, exit_code_val, message_str);

    if(cJSON_IsString(output) && output->valuestring[0] != '\0'){
        log_event("\n\n--- compiler output (Node %d, %s) ---\n%s\n--- end compiler output ---\n\n",
            node_id, source_str, output->valuestring);
        printf("\n\n--- compiler output (Node %d, source=%s, status=%s, exit_code=%d) ---\n%s--- end compiler output ---\n\n",
            node_id, source_str, status_str, exit_code_val, output->valuestring);
    }

    if (should_receive_object && has_object_size) {
        if (remocom_recv_file_stream(client_fd, object_str, object_size_val)) {
            object_received = 1;
            log_event("OBJECT RECEIVED | Node %d | object=%s | bytes=%" PRIu64 "\n",
                node_id, object_str, object_size_val);
            printf("Object received from Node %d: %s (%" PRIu64 " bytes)\n",
                node_id, object_str, object_size_val);
        } else {
            log_event("[ERROR] Failed receiving object file | Node %d | object=%s | bytes=%" PRIu64 "\n",
                node_id, object_str, object_size_val);
            printf("Failed receiving object from Node %d: %s (%" PRIu64 " bytes)\n",
                node_id, object_str, object_size_val);
        }
    } else if (should_receive_object) {
        log_event("[ERROR] Worker reported object without valid object_size | Node %d | object=%s\n",
            node_id, object_str);
    }

    if (compile_succeeded && !object_received) {
        log_event("[ERROR] Successful task did not produce a received object | Node %d | source=%s | object=%s\n",
            node_id, source_str, object_str);
    }

    return record_task_result_for_link(source_str, object_str, compile_succeeded && object_received);
}

/// @brief Handles a parsed worker message after JSON validation.
/// @param client_fd Worker socket.
/// @param type Parsed message type field.
/// @param payload Parsed message payload field.
/// @param is_dead Set to 1 when worker should be disconnected.
static void dispatch_worker_message(int client_fd, cJSON *type, cJSON *payload, int *is_dead) {
    // Look up this worker under lock so heartbeat/status updates are safe.
    pthread_mutex_lock(&workers_mutex);
    Node *worker = find_worker_by_socket_unlocked(client_fd);

    if (worker == NULL) {
        pthread_mutex_unlock(&workers_mutex);
        return;
    }

    int node_id = worker->nodeID;
    NodeStatus status = worker->status;
    int handshake_completed = worker->handshake_completed;

    char *payload_str = cJSON_PrintUnformatted(payload);
    if (payload_str == NULL) {
        payload_str = strdup("<unprintable>");
    }

    if (status == dead) {
        pthread_mutex_unlock(&workers_mutex);

        printf("Received Type: %s | Payload: %s (node already timed out)\n", type->valuestring, payload_str);
        log_event("MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s (node already timed out)\n",
            node_id, type->valuestring, payload_str);

        free(payload_str);
        *is_dead = 1;
        return;
    }

    printf("Received Type: %s | Payload: %s\n", type->valuestring, payload_str);
    log_event("MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s\n",
        node_id, type->valuestring, payload_str);
    free(payload_str);

    if (strcmp(type->valuestring, MSG_TYPE_HANDSHAKE) == 0) {
        char mismatch_reason[256];
        int handshake_ok = validate_handshake_payload(worker, payload, mismatch_reason, sizeof(mismatch_reason));
        pthread_mutex_unlock(&workers_mutex);

        if (handshake_ok) {
            send_json_message(client_fd, MSG_TYPE_HANDSHAKE_ACK, "Handshake accepted");
            log_event("HANDSHAKE ACCEPTED Node %d\n", node_id);
            return;
        }

        send_json_message(client_fd, MSG_TYPE_HANDSHAKE_REJECT, mismatch_reason);
        log_event("HANDSHAKE REJECTED Node %d | Reason: %s\n", node_id, mismatch_reason);
        *is_dead = 1;
        return;
    }

    if (!handshake_completed) {
        pthread_mutex_unlock(&workers_mutex);
        send_json_message(client_fd, MSG_TYPE_HANDSHAKE_REQUIRED, "Handshake required before registration");
        return;
    }

    // Handle heartbeat immediately to keep worker alive in the system.
    if (strcmp(type->valuestring, "heartbeat") == 0) {
        worker->last_heartbeat = time(NULL);
        pthread_mutex_unlock(&workers_mutex);
        return;
    }

    pthread_mutex_unlock(&workers_mutex);

    // Handle other message types outside the lock.
    if (strcmp(type->valuestring, "register") == 0) {
        send_json_message(client_fd, "ack", "Worker registered"); // Acknowledge registration.
    } else if (strcmp(type->valuestring, MSG_TYPE_TASK_REQUEST) == 0) {
        assign_task_to_worker(client_fd, node_id); // Assign next task or report no-task.
    } else if (strcmp(type->valuestring, MSG_TYPE_TASK_RESULT) == 0) {
        int should_link = handle_task_result(client_fd, node_id, payload); // Log the task result reported by the worker.

        // Worker finished this task - clear the active-task marker so a later
        // disconnect doesn't trigger a task reassignment.
        pthread_mutex_lock(&workers_mutex);
        Node *completed_worker = find_worker_by_socket_unlocked(client_fd);
        if(completed_worker != NULL){
            completed_worker->has_active_task = 0;
        }
        pthread_mutex_unlock(&workers_mutex);

        if (should_link) {
            run_link_step();
        }

    } else {
        send_json_message(client_fd, "unknown", "Unknown message type");
    }
}

/// @brief Removes a worker from the global list and logs disconnect reason.
/// @param client_fd Worker socket to remove.
static void remove_worker_by_socket(int client_fd) {
    pthread_mutex_lock(&workers_mutex);

    // Locate the worker.
    for (int i = 0; i < worker_count; i++) {
        Node *worker = &workers[i];

        // Log the disconnect event with reason (timeout vs normal disconnect) and remove the worker by replacing it with the last entry in the list.
        if (worker->socketID == client_fd) {
            if (worker->status == dead) {
                log_event("REMOVED (timeout) Node %d | IP: %s\n", worker->nodeID, worker->ip_address);
                // Monitor already re-queued the task (if any) when it set status = dead.
            } else {
                log_event("DISCONNECT Node %d | IP: %s | Socket: %d\n",
                    worker->nodeID, worker->ip_address, worker->socketID);
                
                // Worker disconnected before monitor saw it - re-queue its task here
                if(worker->has_active_task){
                    log_event("RE-ENQUEUE task for reassignment | Node %d | Source: %s\n",
                    worker->nodeID, worker->current_task.source_path);

                    pthread_mutex_lock(&task_mutex);
                    enqueue_for_reassign_unlocked(&worker->current_task);
                    pthread_mutex_unlock(&task_mutex);

                    worker->has_active_task = 0;
                }
            }

            *worker = workers[worker_count - 1];
            worker_count--;
            break;
        }
    }

    printf("Worker removed. Total workers: %d\n", worker_count);
    pthread_mutex_unlock(&workers_mutex);
}

/// @brief Registers a newly connected worker or rejects it if capacity is full.
/// @param client_fd Worker socket.
/// @param client_addr Remote address for logging.
/// @return 1 if worker is registered, 0 if rejected.
static int register_worker(int client_fd, const struct sockaddr_in *client_addr) {
    // Register this socket as a worker unless we are at max capacity.
    pthread_mutex_lock(&workers_mutex);

    // Check if we have room for another worker. If not, reject the connection and log the event.
    if (worker_count >= MAX_WORKERS) {
        pthread_mutex_unlock(&workers_mutex);
        log_event("[WARNING] Rejecting connection: max worker limit reached\n");
        close(client_fd);
        return 0;
    }

    // Create new worker.
    Node *worker = &workers[worker_count];
    worker->socketID = client_fd;
    worker->nodeID = worker_count;
    worker->last_heartbeat = time(NULL);
    worker->status = alive;
    worker->handshake_completed = 0;
    worker->has_active_task = 0;
    // current_task left uninitialized - unsed until has_active_task becomes 1

    // Convert the client's IP address to a string and store it in the worker struct.
    inet_ntop(AF_INET, &client_addr->sin_addr, worker->ip_address, INET_ADDRSTRLEN);

    printf("Worker added. Total workers: %d\n", worker_count + 1);
    log_event("CONNECT Node %d | IP: %s | Socket: %d\n",
        worker->nodeID, worker->ip_address, worker->socketID);

    worker_count++;
    pthread_mutex_unlock(&workers_mutex);
    return 1;
}

/// @brief Creates and detaches a handler thread for a worker connection.
/// @param client_fd_ptr Heap pointer containing worker socket.
/// @return 1 on success, 0 on failure.
static int spawn_worker_thread(int *client_fd_ptr) {
    pthread_t thread_id;

    // Create a new thread to handle this worker connection.
    // If thread creation fails, clean up by removing the worker from the list, closing the socket, and logging the error.
    if (pthread_create(&thread_id, NULL, handle_worker, client_fd_ptr) != 0) {
        perror("Thread creation failed");

        pthread_mutex_lock(&workers_mutex);
        Node *worker = find_worker_by_socket_unlocked(*client_fd_ptr);
        if (worker != NULL) {
            log_event("[ERROR] Failed to connect Node %d to thread.\n", worker->nodeID);
            *worker = workers[worker_count - 1];
            worker_count--;
        }
        pthread_mutex_unlock(&workers_mutex);

        close(*client_fd_ptr);
        free(client_fd_ptr);
        return 0;
    }

    // let thread clean itself up after finishing so we dont have to track it
    pthread_detach(thread_id);
    return 1;
}

/// @brief Creates, binds, and listens on the coordinator server socket.
/// @return Server socket file descriptor.
static int create_server_socket(void) {
    // this creates the server or "phone" by using AF_INET (IPv4) and SOCK_STREAM (TCP)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket failed");
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    // Set up the server address info (defines where the server lives)
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT); // port == a "channel" people connect to

    // bind is saying "hey OS, i want to use port 5000"
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(1);
    }

    // listen means "im ready for incoming connections" and 5 means 5 can wait in line
    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(1);
    }

    return server_fd;
}

/// @brief Checks in with live worker nodes every 10 seconds, marking them as dead if they haven't sent a heartbeat within the last 15 seconds and logging timeouts.
static void *monitor_workers(void *arg) {
    (void)arg;

    while (1) {
        sleep(10);
        time_t current_time = time(NULL);

        pthread_mutex_lock(&workers_mutex); // Lock worker list.
        // Loop through all live workers and check for timeouts.
        for (int i = 0; i < worker_count; i++) {
            Node *worker = &workers[i];
            int time_since_heartbeat = (int)(current_time - worker->last_heartbeat);

            if (time_since_heartbeat > 15 && worker->status != dead) {
                // Log the timeout event.
                log_event("[TIMEOUT] Node %d | IP: %s\n", worker->nodeID, worker->ip_address);
                // Mark the worker as dead and close its socket.
                worker->status = dead;
                close(worker->socketID);

                // If this worker was mid-task, push the task back for reassignment
                if(worker->has_active_task){
                    log_event("RE-ENQUEUE task for reassignment | Node %d | Source: %s\n",
                    worker->nodeID, worker->current_task.source_path);

                    pthread_mutex_lock(&task_mutex);
                    enqueue_for_reassign_unlocked(&worker->current_task);
                    pthread_mutex_unlock(&task_mutex);

                    worker->has_active_task = 0;
                }
            }
        }
        pthread_mutex_unlock(&workers_mutex); // Unlock worker list.
    }

    return NULL;
}

/// @brief Sends a JSON message to a connected worker.
/// @param client_fd The socket file descriptor of the worker to send the message to.
/// @param type_str The type of the JSON message.
/// @param payload_str The payload of the JSON message.
static void send_json_message(int client_fd, const char *type_str, const char *payload_str) {
    remocom_send_json_message(client_fd, type_str, payload_str);
}

/// @brief Handles communication with a connected worker node, processing incoming JSON messages, updating worker status based on heartbeats, assigning tasks, and logging events. This function runs in a separate thread for each worker connection.
/// @param arg A pointer to the client socket file descriptor.
/// @return NULL when the worker disconnects or an error occurs.
static void *handle_worker(void *arg) {
    int is_dead = 0;
    int client_fd = *(int *)arg; //get the client socket passed from main thread
    free(arg); // free memory after grabbing value

    printf("Helper handling worker\n");

    cJSON *msg = remocom_recv_json_message(client_fd);
    while (msg != NULL) { // connection stays alive as long as messages are being read

        // Check if the received message is valid JSON. If not, log and close.
        if (msg == NULL) {
            printf("Invalid JSON received\n");
            log_event("[WARNING] Invalid JSON received\n");
            close(client_fd);
            return NULL;
        }

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        cJSON *payload = cJSON_GetObjectItem(msg, "payload");

        // Make sure message has the required fields in string form.
        if (type == NULL || payload == NULL || !cJSON_IsString(type)) {
            printf("Missing fields in JSON\n");
            log_event("[WARNING] Missing fields in JSON message\n");
            cJSON_Delete(msg);
            close(client_fd);
            return NULL;
        }

        // Handle heartbeat/register/task request/unknown types.
        dispatch_worker_message(client_fd, type, payload, &is_dead);

        cJSON_Delete(msg);
        if (is_dead) {
            break;
        }

        msg = remocom_recv_json_message(client_fd);
    }

    if (!is_dead) {
        printf("Worker disconnected\n");
    }

    remove_worker_by_socket(client_fd);
    close(client_fd);
    printf("Helper finished worker\n");
    return NULL;
}

/// @brief Initializes the coordinator server, loads manifest tasks, sets up the listening socket, and handles incoming worker connections by spawning a new thread for each worker. It also starts a monitoring thread to check for worker heartbeats and logs all significant events to a log file.
int main(int argc, char **argv) {
    const char *manifest_path = NULL;
    char manifest_error[256];

    if (!parse_manifest_cli_flag(argc, argv, &manifest_path)) {
        print_usage(argv[0]);
        return 1;
    }

    if (!load_manifest_file(manifest_path, &build_manifest, manifest_error, sizeof(manifest_error))) {
        fprintf(stderr, "%s\n", manifest_error);
        return 1;
    }

    if (!change_to_manifest_directory(manifest_path, manifest_error, sizeof(manifest_error))) {
        fprintf(stderr, "%s\n", manifest_error);
        return 1;
    }

    if (!build_compile_tasks_from_manifest(&build_manifest, manifest_error, sizeof(manifest_error))) {
        fprintf(stderr, "%s\n", manifest_error);
        return 1;
    }

    int server_fd;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Open log file for writing coordinator events and errors.
    log_file = fopen("coordinator.log", "w");
    if (log_file == NULL) {
        perror("Failed to open log file");
        return 1;
    }

    log_event("MANIFEST LOADED | output=%s | sources=%d | headers=%d | flags=%d\n",
        build_manifest.output, build_manifest.source_count, build_manifest.header_count, build_manifest.flag_count);

    server_fd = create_server_socket();

    // Create thread that will monitor heartbeat status of nodes.
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitor_workers, NULL);
    pthread_detach(monitor_thread);

    printf("Coordinator listening on port %d\n", PORT);
    printf("Loaded %d compile tasks for output '%s'\n", total_tasks, build_manifest.output);

    // Keep the coordinator running so workers can connect (keeps server alive forever. without this it would only accept one connection and exit)
    while (1) {
        int *client_fd_ptr = malloc(sizeof(int)); //allocate memory so each thread gets its own copy of the socket
        if (client_fd_ptr == NULL) {
            perror("Malloc failed");
            continue;
        }

        // Wait here until a worker connects
        *client_fd_ptr = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len); //accept() = someone calls you
        if (*client_fd_ptr < 0) {
            perror("Accept failed");
            free(client_fd_ptr);
            continue;
        }

        // Register this worker in the system and log the connection. If registration fails, clean up.
        if (!register_worker(*client_fd_ptr, &client_addr)) {
            free(client_fd_ptr);
            continue;
        }

        //after accept you now have two sockets Server_fd which keeps listening and client_fd which handles this connection 
        //exmaple of this is one phone stays on desk (server_fd) and one phone is in your hand (client_fd)
        printf("Worker connected - transferring to helper\n");

        //create a new thread (helper) to handle this worker so the main server can go back to answering calls
        if (!spawn_worker_thread(client_fd_ptr)) {
            continue;
        }

        //main thread immediately goes back to accept() to wait for the next worker
        printf("Main ready for next worker\n");
    }

    close(server_fd);
    return 0;
}
