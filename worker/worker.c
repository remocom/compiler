#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <cjson/cJSON.h>
#include "../common/common.h"

#define PORT 5000
#define BUFFER_SIZE 4096
#define MAX_TASK_FLAGS 64
#define DISCARD_BUFFER_SIZE 4096
#define MAX_COMPILER_ARGS (MAX_TASK_FLAGS * 3 + 10)
#define WORKER_PATH_SIZE 512
#define WORKER_LOCAL_OBJECT_SIZE (WORKER_PATH_SIZE + 32)
#define WORKER_STATUS_SIZE 512
#define WORKER_COMPILER_OUTPUT_SIZE 2048

/// @brief Structure to hold the state of a task execution within the worker.
typedef struct {
    cJSON *payload;
    cJSON *source_json;
    cJSON *object_json;
    cJSON *files_json;
    char source[WORKER_PATH_SIZE];
    char object[WORKER_PATH_SIZE];
    char task_dir[WORKER_PATH_SIZE];
    char local_source[WORKER_PATH_SIZE];
    char local_object[WORKER_LOCAL_OBJECT_SIZE];
    char status_message[WORKER_STATUS_SIZE];
    char compiler_output[WORKER_COMPILER_OUTPUT_SIZE];
    int exit_code;
    int success;
    int task_dir_created;
} WorkerTaskExecution;

/// @brief Creates a new socket for the worker to communicate with the coordinator.
/// @return The file descriptor for the created socket.
static int create_worker_socket(void) {
    // this creates the worker "phone" socket using IPv4 + TCP
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket failed");
        exit(1);
    }

    return sock_fd;
}

/// @brief Connects the worker socket to the coordinator.
/// @param sock_fd The file descriptor for the worker socket.
static void connect_to_coordinator(int sock_fd) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    // set up coordinator address (where the worker is dialing)
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connect failed");
        close(sock_fd);
        exit(1);
    }
}

/// @brief Sends a JSON message with an arbitrary payload to the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param type The type of the message.
/// @param payload The payload object/value to attach.
static void send_json_with_payload(int sock_fd, const char *type, cJSON *payload) {
    remocom_send_json_with_payload(sock_fd, type, payload);
}

/// @brief Sends a JSON message to the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param type The type of the message.
/// @param payload The payload of the message.
static void send_json_message(int sock_fd, const char *type, const char *payload) {
    remocom_send_json_message(sock_fd, type, payload);
}

/// @brief Sends a handshake payload describing worker build/runtime compatibility.
/// @param sock_fd The file descriptor for the coordinator socket.
static void send_handshake_message(int sock_fd) {
    cJSON *payload = cJSON_CreateObject();

    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_GCC_VERSION, __VERSION__);
    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_TARGET_ARCH, remocom_detect_target_arch());
    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_TARGET_OS, remocom_detect_target_os());
    cJSON_AddNumberToObject(payload, HANDSHAKE_KEY_RPC_PROTOCOL_VERSION, REMOCOM_RPC_PROTOCOL_VERSION);

    remocom_send_json_with_payload(sock_fd, MSG_TYPE_HANDSHAKE, payload);
}

/// @brief Returns the relative path of a transfer file, removing leading slashes.
/// @param path The absolute path of the transfer file.
/// @return The relative path of the transfer file.
static const char *relative_transfer_path(const char *path) {
    while (*path == '/') {
        path++;
    }

    return path;
}

/// @brief Constructs the full path for an include file within the task directory.
/// @param task_dir The directory for the current task.
/// @param include_path The path of the include file.
/// @param out A buffer to store the constructed path.
/// @param out_size The size of the output buffer.
static void make_task_include_path(const char *task_dir, const char *include_path, char *out, size_t out_size) {
    const char *relative_path = relative_transfer_path(include_path);
    snprintf(out, out_size, "%s/%s", task_dir, relative_path);
}

/// @brief Recursively removes a directory and all its contents.
/// @param path The path of the directory to remove.
static void cleanup_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        unlink(path);
        return;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[512];
        snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        cleanup_tree(child_path);
    }

    closedir(dir);
    rmdir(path);
}

/// @brief Discards a stream of a given size from a file descriptor.
/// @param sock_fd The file descriptor from which to discard the stream.
/// @param size The size of the stream to discard.
/// @return 1 if successful, 0 otherwise.
static int discard_stream(int sock_fd, uint64_t size) {
    char discard_buffer[DISCARD_BUFFER_SIZE];
    uint64_t remaining = size;

    while (remaining > 0) {
        size_t chunk_size = remaining < sizeof(discard_buffer) ? (size_t)remaining : sizeof(discard_buffer);
        if (!remocom_recv_all(sock_fd, discard_buffer, chunk_size)) {
            return 0;
        }
        remaining -= chunk_size;
    }

    return 1;
}

/// @brief Discards files from a task payload that the worker is not going to process.
/// @param sock_fd The file descriptor from which to discard the files.
/// @param files The array of files to discard.
static void discard_task_files(int sock_fd, cJSON *files) {
    if (!cJSON_IsArray(files)) {
        return;
    }

    int file_count = cJSON_GetArraySize(files);
    for (int i = 0; i < file_count; i++) {
        cJSON *file_item = cJSON_GetArrayItem(files, i);
        cJSON *size_json = cJSON_GetObjectItem(file_item, TASK_KEY_SIZE);
        uint64_t file_size = 0;
        if (remocom_parse_u64_string(size_json, &file_size)) {
            discard_stream(sock_fd, file_size);
        }
    }
}

/// @brief Receives the files for a task from the coordinator.
/// @param sock_fd The file descriptor from which to receive the files.
/// @param files The array of files to receive.
/// @param task_dir The directory for the current task.
/// @param source_path The path of the source file.
/// @param local_source A buffer to store the path of the local source file.
/// @param local_source_size The size of the local source buffer.
/// @param status_message A buffer to store any error messages.
/// @param status_message_size The size of the status message buffer.
/// @return 1 if successful, 0 otherwise.
static int receive_task_files(int sock_fd, cJSON *files, const char *task_dir, const char *source_path,
    char *local_source, size_t local_source_size, char *status_message, size_t status_message_size) {
    if (!cJSON_IsArray(files)) {
        snprintf(status_message, status_message_size, "Task payload missing files array");
        return 0;
    }

    int ok = 1;
    int file_count = cJSON_GetArraySize(files);
    for (int i = 0; i < file_count; i++) {
        cJSON *file_item = cJSON_GetArrayItem(files, i);
        cJSON *path_json = cJSON_GetObjectItem(file_item, TASK_KEY_PATH);
        cJSON *size_json = cJSON_GetObjectItem(file_item, TASK_KEY_SIZE);
        uint64_t file_size = 0;

        if (!cJSON_IsObject(file_item) || !cJSON_IsString(path_json) ||
            !remocom_parse_u64_string(size_json, &file_size)) {
            snprintf(status_message, status_message_size, "Invalid file metadata in task payload");
            return 0;
        }

        const char *relative_path = relative_transfer_path(path_json->valuestring);
        char local_path[512];
        snprintf(local_path, sizeof(local_path), "%s/%s", task_dir, relative_path);

        if (!remocom_recv_file_stream(sock_fd, local_path, file_size)) {
            if (ok) {
                snprintf(status_message, status_message_size, "Failed receiving task file: %s", path_json->valuestring);
            }
            return 0;
        }

        if (strcmp(path_json->valuestring, source_path) == 0) {
            snprintf(local_source, local_source_size, "%s", local_path);
        }
    }

    if (ok && local_source[0] == '\0') {
        snprintf(status_message, status_message_size, "Task files did not include source: %s", source_path);
        return 0;
    }

    return ok;
}

/// @brief Performs compatibility handshake and returns 1 on success.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer Reusable receive buffer.
/// @return 1 if handshake accepted, 0 otherwise.
static int perform_handshake(int sock_fd, char *buffer) {
    (void)buffer;
    send_handshake_message(sock_fd);

    cJSON *msg = remocom_recv_json_message(sock_fd);
    if (msg == NULL) {
        return 0;
    }

    cJSON *type = cJSON_GetObjectItem(msg, RPC_KEY_TYPE);
    int accepted = cJSON_IsString(type) && strcmp(type->valuestring, MSG_TYPE_HANDSHAKE_ACK) == 0;

    if (!accepted) {
        char *response = cJSON_PrintUnformatted(msg);
        printf("Handshake failed: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
        cJSON_Delete(msg);
        return 0;
    }

    printf("Handshake accepted by coordinator\n");
    cJSON_Delete(msg);
    return 1;
}

/// @brief Registers the worker with the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer The buffer to receive data into.
static void register_with_coordinator(int sock_fd, char *buffer) {
    (void)buffer;
    // send initial register message so coordinator knows this worker joined
    send_json_message(sock_fd, MSG_TYPE_REGISTER, MSG_PAYLOAD_WORKER_REGISTER);

    cJSON *msg = remocom_recv_json_message(sock_fd);
    if (msg != NULL) {
        char *response = cJSON_PrintUnformatted(msg);
        printf("Received from coordinator: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
        cJSON_Delete(msg);
    }
}

/// @brief Initializes a worker task execution structure.
/// @param task The task execution structure to initialize.
/// @param payload The JSON payload containing the task information.
static void init_task_execution(WorkerTaskExecution *task, cJSON *payload) {
    memset(task, 0, sizeof(*task));
    task->payload = payload;
    task->exit_code = 1;
    snprintf(task->task_dir, sizeof(task->task_dir), "/tmp/remocom-worker-XXXXXX");
}

/// @brief Sets the status message for a worker task execution.
/// @param task The task execution structure for which to set the status.
/// @param format The format string for the status message.
/// @param ... The arguments for the format string.
static void set_task_status(WorkerTaskExecution *task, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(task->status_message, sizeof(task->status_message), format, args);
    va_end(args);
}

/// @brief Parses the task assignment payload and initializes the task execution structure.
/// @param task The task execution structure to initialize.
/// @return 1 if parsing is successful, 0 otherwise.
static int parse_task_assignment_payload(WorkerTaskExecution *task) {
    task->source_json = cJSON_GetObjectItem(task->payload, TASK_KEY_SOURCE);
    task->object_json = cJSON_GetObjectItem(task->payload, TASK_KEY_OBJECT);
    task->files_json = cJSON_GetObjectItem(task->payload, TASK_KEY_FILES);

    if (cJSON_IsString(task->source_json)) {
        snprintf(task->source, sizeof(task->source), "%s", task->source_json->valuestring);
    }
    if (cJSON_IsString(task->object_json)) {
        snprintf(task->object, sizeof(task->object), "%s", task->object_json->valuestring);
    }

    if (!cJSON_IsString(task->source_json)) {
        set_task_status(task, "Task payload missing source");
        return 0;
    }

    return 1;
}

/// @brief Creates a temporary directory for the task execution.
/// @param task The task execution structure for which to create the directory.
/// @return 1 if the directory is created successfully, 0 otherwise.
static int create_task_directory(WorkerTaskExecution *task) {
    if (mkdtemp(task->task_dir) == NULL) {
        set_task_status(task, "mkdtemp() failed");
        return 0;
    }

    task->task_dir_created = 1;
    snprintf(task->local_object, sizeof(task->local_object), "%s/output.o", task->task_dir);
    return 1;
}

/// @brief Receives the input files for the task execution.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param task The task execution structure for which to receive inputs.
/// @return 1 if the inputs are received successfully, 0 otherwise.
static int receive_task_inputs(int sock_fd, WorkerTaskExecution *task) {
    return receive_task_files(
        sock_fd,
        task->files_json,
        task->task_dir,
        task->source_json->valuestring,
        task->local_source,
        sizeof(task->local_source),
        task->status_message,
        sizeof(task->status_message)
    );
}

/// @brief Appends include arguments for the task compilation.
/// @param flags_json The JSON array containing the compilation flags.
/// @param flag_index The index of the current flag.
/// @param flag_value The value of the current flag.
/// @param task The task execution structure.
/// @param argv The argument vector for the compiler command.
/// @param argc A pointer to the argument count.
/// @param task_include_paths An array to store the include paths.
/// @param task_include_count A pointer to the count of include paths.
static void append_task_include_args(
    cJSON *flags_json,
    int flag_index,
    const char *flag_value,
    const WorkerTaskExecution *task,
    char **argv,
    int *argc,
    char task_include_paths[MAX_TASK_FLAGS][WORKER_PATH_SIZE],
    int *task_include_count
) {
    if (strcmp(flag_value, "-I") == 0 && flag_index + 1 < cJSON_GetArraySize(flags_json)) {
        cJSON *include_dir = cJSON_GetArrayItem(flags_json, flag_index + 1);
        if (cJSON_IsString(include_dir) && *task_include_count < MAX_TASK_FLAGS) {
            argv[(*argc)++] = "-I";
            make_task_include_path(task->task_dir, include_dir->valuestring,
                task_include_paths[*task_include_count], WORKER_PATH_SIZE);
            argv[(*argc)++] = task_include_paths[*task_include_count];
            (*task_include_count)++;
        }
    } else if (strncmp(flag_value, "-I", 2) == 0 && flag_value[2] != '\0' &&
        *task_include_count < MAX_TASK_FLAGS) {
        make_task_include_path(task->task_dir, flag_value + 2,
            task_include_paths[*task_include_count], WORKER_PATH_SIZE);
        argv[(*argc)++] = "-I";
        argv[(*argc)++] = task_include_paths[*task_include_count];
        (*task_include_count)++;
    }

}

/// @brief Builds the argument vector for the compiler command.
/// @param task The task execution structure.
/// @param argv The argument vector for the compiler command.
/// @param task_include_paths An array to store the include paths.
/// @param compiler_driver_out A pointer to the compiler driver string.
/// @return 1 if the argument vector is built successfully, 0 otherwise.
static int build_compile_argv(
    WorkerTaskExecution *task,
    char **argv,
    char task_include_paths[MAX_TASK_FLAGS][WORKER_PATH_SIZE],
    const char **compiler_driver_out
) {
    cJSON *flags_json = cJSON_GetObjectItem(task->payload, TASK_KEY_FLAGS);

    if (!cJSON_IsString(task->source_json) || !cJSON_IsString(task->object_json) || !cJSON_IsArray(flags_json)) {
        set_task_status(task, "Task payload missing source/object/flags");
        task->exit_code = 1;
        return 0;
    }

    snprintf(task->source, sizeof(task->source), "%s", task->source_json->valuestring);
    snprintf(task->object, sizeof(task->object), "%s", task->object_json->valuestring);
    const char *compiler_driver = remocom_select_source_driver(task->source_json->valuestring);
    *compiler_driver_out = compiler_driver;

    int task_include_count = 0;
    int argc = 0;
    argv[argc++] = (char *)compiler_driver;

    int flag_count = cJSON_GetArraySize(flags_json);
    if (flag_count > MAX_TASK_FLAGS) {
        set_task_status(task, "Too many flags in task payload");
        task->exit_code = 1;
        return 0;
    }

    for (int i = 0; i < flag_count; i++) {
        cJSON *flag = cJSON_GetArrayItem(flags_json, i);
        if (!cJSON_IsString(flag)) {
            set_task_status(task, "Flag at index %d is not a string", i);
            task->exit_code = 1;
            return 0;
        }
        argv[argc++] = flag->valuestring;

        append_task_include_args(flags_json, i, flag->valuestring, task, argv, &argc,
            task_include_paths, &task_include_count);
    }

    argv[argc++] = "-I";
    argv[argc++] = task->task_dir;

    // Add source and object arguments.
    argv[argc++] = "-c";
    argv[argc++] = task->local_source;
    argv[argc++] = "-o";
    argv[argc++] = task->local_object;
    argv[argc] = NULL;
    return 1;
}

/// @brief Executes a compile task received from the coordinator.
/// @param task The task execution structure containing the task details.
/// @return 1 if the compilation succeeded, 0 otherwise.
static int run_compile_task(WorkerTaskExecution *task) {
    char *argv[MAX_COMPILER_ARGS];
    char task_include_paths[MAX_TASK_FLAGS][WORKER_PATH_SIZE];
    const char *compiler_driver = NULL;

    if (!build_compile_argv(task, argv, task_include_paths, &compiler_driver)) {
        return 0;
    }

    int status = 0;
    if (!remocom_run_process_capture(
        argv,
        task->compiler_output,
        sizeof(task->compiler_output),
        &status
    )) {
        set_task_status(task, "Compiler process failed: %s", strerror(errno));
        task->exit_code = 1;
        return 0;
    }

    // Check if the compiler exited normally and capture the exit code.
    // Construct a human-readable status message based on the result.
    if (WIFEXITED(status)) {
        task->exit_code = WEXITSTATUS(status);
        if (task->exit_code == 0) {
            set_task_status(task, "Compiled %s -> %s with %s",
                task->source, task->object, compiler_driver);
            return 1;
        }

        set_task_status(task, "%s failed for %s with exit code %d",
            compiler_driver, task->source, task->exit_code);
        return 0;
    }

    set_task_status(task, "%s terminated abnormally for %s",
        compiler_driver, task->source);
    task->exit_code = 1;
    return 0;
}

/// @brief Sends compilation result details back to the coordinator.
/// @param sock_fd Coordinator socket.
/// @param source Source file path for the task.
/// @param object Object file path for the task.
/// @param status success/failure text.
/// @param exit_code GCC exit code.
/// @param message Human-readable task status message.
/// @param output Captured stdout/stderr from the compiler process.
static void send_task_result(int sock_fd, const char *source, const char *object, const char *status,
    int exit_code, const char *message, const char *output, const char *local_object, int has_object) {
    uint64_t object_size = 0;
    if (has_object && !remocom_get_file_size(local_object, &object_size)) {
        has_object = 0;
        object_size = 0;
    }

    char object_size_text[32];
    snprintf(object_size_text, sizeof(object_size_text), "%" PRIu64, object_size);

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, TASK_KEY_SOURCE, source);
    cJSON_AddStringToObject(payload, TASK_KEY_OBJECT, object);
    cJSON_AddStringToObject(payload, TASK_KEY_STATUS, status);
    cJSON_AddNumberToObject(payload, TASK_KEY_EXIT_CODE, exit_code);
    cJSON_AddStringToObject(payload, TASK_KEY_MESSAGE, message);
    cJSON_AddStringToObject(payload, TASK_KEY_COMPILER_OUTPUT, output);
    cJSON_AddBoolToObject(payload, TASK_KEY_HAS_OBJECT, has_object);
    cJSON_AddStringToObject(payload, TASK_KEY_OBJECT_SIZE, object_size_text);

    send_json_with_payload(sock_fd, MSG_TYPE_TASK_RESULT, payload);
    if (has_object) {
        remocom_send_file_stream(sock_fd, local_object);
    }
}

/// @brief Sends the result of a task execution back to the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param task The task execution structure containing the result details.
static void send_task_execution_result(int sock_fd, const WorkerTaskExecution *task) {
    send_task_result(
        sock_fd,
        task->source,
        task->object,
        task->success ? TASK_STATUS_SUCCESS : TASK_STATUS_FAILURE,
        task->exit_code,
        task->status_message,
        task->compiler_output,
        task->local_object,
        task->success
    );
}

/// @brief Processes a task assignment message from the coordinator, executing the compile
/// and reporting the result.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param payload The JSON payload containing the task assignment details.
static void process_task_assignment(int sock_fd, cJSON *payload) {
    WorkerTaskExecution task;
    init_task_execution(&task, payload);

    if (!parse_task_assignment_payload(&task)) {
        discard_task_files(sock_fd, task.files_json);
    } else if (!create_task_directory(&task)) {
        discard_task_files(sock_fd, task.files_json);
    } else if (receive_task_inputs(sock_fd, &task)) {
        task.success = run_compile_task(&task);
    }

    printf("Task completed: %s\n", task.status_message);
    send_task_execution_result(sock_fd, &task);

    if (task.task_dir_created) {
        cleanup_tree(task.task_dir);
    }
}

/// @brief Requests one task from coordinator and processes it.
/// @param sock_fd Coordinator socket.
/// @param buffer Shared receive buffer.
/// @return 1 if worker should continue requesting tasks, 0 otherwise.
static int request_and_process_task(int sock_fd, char *buffer) {
    (void)buffer;
    send_json_message(sock_fd, MSG_TYPE_TASK_REQUEST, MSG_PAYLOAD_TASK_REQUEST);

    cJSON *msg = remocom_recv_json_message(sock_fd);
    if (msg == NULL) {
        printf("Invalid task response\n");
        return 0;
    }

    cJSON *type = cJSON_GetObjectItem(msg, RPC_KEY_TYPE);
    cJSON *payload = cJSON_GetObjectItem(msg, RPC_KEY_PAYLOAD);
    if (!cJSON_IsString(type) || payload == NULL) {
        cJSON_Delete(msg);
        return 0;
    }

    if (strcmp(type->valuestring, MSG_TYPE_NO_TASK) == 0) {
        printf("Coordinator reported no more tasks\n");
        cJSON_Delete(msg);
        return 0;
    }

    if (strcmp(type->valuestring, MSG_TYPE_TASK_ERROR) == 0) {
        char *response = cJSON_PrintUnformatted(payload);
        printf("Coordinator could not prepare task: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
        cJSON_Delete(msg);
        return 0;
    }

    // For task assignments, run the compile and report the result back to the coordinator.
    if (strcmp(type->valuestring, MSG_TYPE_TASK_ASSIGNMENT) == 0) {
        process_task_assignment(sock_fd, payload);
    } else {
        char *response = cJSON_PrintUnformatted(msg);
        printf("Unexpected response from coordinator: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
    }

    cJSON_Delete(msg);
    return 1;
}

/// @brief Continuously requests and executes tasks until coordinator has no more work.
/// @param sock_fd Coordinator socket.
/// @param buffer Shared receive buffer.
static void execute_task_loop(int sock_fd, char *buffer) {
    while (request_and_process_task(sock_fd, buffer)) {
    }
}

/// @brief Sends heartbeats to the coordinator at regular intervals.
/// @param sock_fd The file descriptor for the coordinator socket.
static void heartbeat_loop(int sock_fd) {
    while (1) {
        sleep(5); // every 5 seconds send heartbeat

        // heartbeat message tells coordinator this worker is still alive
        send_json_message(sock_fd, MSG_TYPE_HEARTBEAT, MSG_PAYLOAD_HEARTBEAT_ALIVE);
        printf("Heartbeat sent\n");
    }
}

/// @brief The main entry point for the worker process.
/// @return The exit status of the program.
int main(void) {
    char buffer[BUFFER_SIZE];
    int sock_fd = create_worker_socket();

    // connect() == worker dials coordinator
    connect_to_coordinator(sock_fd);
    printf("Connected to coordinator\n");

    // Perform handshake to verify compatibility with coordinator before proceeding.
    if (!perform_handshake(sock_fd, buffer)) {
        printf("Unable to join compilation network\n");
        close(sock_fd);
        return 1;
    }

    // Register with the coordinator, request tasks, then keep sending heartbeats in a loop.
    register_with_coordinator(sock_fd, buffer);
    execute_task_loop(sock_fd, buffer);
    heartbeat_loop(sock_fd);

    close(sock_fd);
    return 0;
}
