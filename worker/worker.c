#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cjson/cJSON.h>
#include "../common/common.h"

#define PORT 5000
#define BUFFER_SIZE 4096
#define MAX_TASK_FLAGS 64
#define DISCARD_BUFFER_SIZE 4096
#define MAX_COMPILER_ARGS (MAX_TASK_FLAGS * 3 + 10)

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

/// @brief Parses a JSON string into a uint64_t value.
/// @param value The JSON string to parse.
/// @param out A pointer to the uint64_t variable where the parsed value will be stored.
/// @return 1 if successful, 0 otherwise.
static int parse_u64_string(cJSON *value, uint64_t *out) {
    if (!cJSON_IsString(value)) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value->valuestring, &end, 10);
    if (errno != 0 || end == value->valuestring || *end != '\0') {
        return 0;
    }

    *out = (uint64_t)parsed;
    return 1;
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

/// @brief Checks whether a source path should use the C++ compiler driver.
/// @param source_path Source path from the task payload.
/// @return 1 for common C++ source extensions, 0 otherwise.
static int is_cpp_source_path(const char *source_path) {
    const char *extension = strrchr(source_path, '.');
    if (extension == NULL) {
        return 0;
    }

    return strcmp(extension, ".cpp") == 0 ||
        strcmp(extension, ".cc") == 0 ||
        strcmp(extension, ".cxx") == 0 ||
        strcmp(extension, ".C") == 0;
}

/// @brief Selects the compiler driver for a source file.
/// @return "g++" for C++ sources, otherwise "gcc".
static const char *select_source_driver(const char *source_path) {
    return is_cpp_source_path(source_path) ? "g++" : "gcc";
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
        cJSON *size_json = cJSON_GetObjectItem(file_item, "size");
        uint64_t file_size = 0;
        if (parse_u64_string(size_json, &file_size)) {
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
        cJSON *path_json = cJSON_GetObjectItem(file_item, "path");
        cJSON *size_json = cJSON_GetObjectItem(file_item, "size");
        uint64_t file_size = 0;

        if (!cJSON_IsObject(file_item) || !cJSON_IsString(path_json) ||
            !parse_u64_string(size_json, &file_size)) {
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

    cJSON *type = cJSON_GetObjectItem(msg, "type");
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
    send_json_message(sock_fd, "register", "Hello from worker");

    cJSON *msg = remocom_recv_json_message(sock_fd);
    if (msg != NULL) {
        char *response = cJSON_PrintUnformatted(msg);
        printf("Received from coordinator: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
        cJSON_Delete(msg);
    }
}

/// @brief Allows parent to read from port that Child wrote to.
/// @param read_fd Read-end of the pipe
/// @param compiler_output Buffer that stores info from the pipe.
/// @param compiler_output_size size of compiler_output buffer
static void read_from_pipe(int read_fd, char *compiler_output, size_t compiler_output_size){
    size_t total= 0;
    ssize_t n = 0;
    int truncated = 0;
    char discard_buf[512]; // used to help drain the buffer in the event compiler_output gets filled to the max and additional bytes still remain in pipe.

    while(1){
        if(!truncated && compiler_output_size > 1){
            size_t space_left = (compiler_output_size - 1) - total;
            if (space_left == 0){
                truncated = 1;
                continue;
            }
            n = read(read_fd, compiler_output + total, space_left);
            if(n > 0){
                total += (size_t)n;
                if(total >= compiler_output_size -1){
                    truncated = 1; // stop storing but keep draining pipe
                }
                continue;
            }
        } else{
            n = read(read_fd, discard_buf, sizeof(discard_buf));
            if(n > 0){
                truncated = 1;
                continue;
            }
        }
        break;
    }

    if(compiler_output_size > 0){
        compiler_output[total] = '\0';
    }
}

/// @brief Executes a compile task received from the coordinator by invoking the appropriate compiler driver.
/// @param payload Parsed task payload containing source/object/flags.
/// @param source Output buffer for source path.
/// @param source_size Size of source buffer.
/// @param object Output buffer for object path.
/// @param object_size Size of object buffer.
/// @param status_message Output buffer for human-readable result.
/// @param status_message_size Size of status buffer.
/// @param compiler_output Buffer that receives captured stdout/stderr from the compiler process.
/// @param compiler_output_size Size of output buffer.
/// @param exit_code_out Exit code from the compiler driver or local failure.

/// @return 1 on successful compile, 0 on failure.
static int run_compile_task(
    cJSON *payload,
    const char *local_source,
    const char *local_object,
    const char *task_dir,
    char *source,
    size_t source_size,
    char *object,
    size_t object_size,
    char *status_message,
    size_t status_message_size,
    char *compiler_output,
    size_t compiler_output_size,
    int *exit_code_out
) {
    cJSON *source_json = cJSON_GetObjectItem(payload, "source");
    cJSON *object_json = cJSON_GetObjectItem(payload, "object");
    cJSON *flags_json = cJSON_GetObjectItem(payload, "flags");

    if (!cJSON_IsString(source_json) || !cJSON_IsString(object_json) || !cJSON_IsArray(flags_json)) {
        snprintf(status_message, status_message_size, "Task payload missing source/object/flags");
        *exit_code_out = 1;
        return 0;
    }

    snprintf(source, source_size, "%s", source_json->valuestring);
    snprintf(object, object_size, "%s", object_json->valuestring);
    const char *compiler_driver = select_source_driver(source_json->valuestring);

    char *argv[MAX_COMPILER_ARGS];
    char task_include_paths[MAX_TASK_FLAGS][512];
    int task_include_count = 0;
    int argc = 0;
    argv[argc++] = (char *)compiler_driver;

    int flag_count = cJSON_GetArraySize(flags_json);
    if (flag_count > MAX_TASK_FLAGS) {
        snprintf(status_message, status_message_size, "Too many flags in task payload");
        *exit_code_out = 1;
        return 0;
    }

    for (int i = 0; i < flag_count; i++) {
        cJSON *flag = cJSON_GetArrayItem(flags_json, i);
        if (!cJSON_IsString(flag)) {
            snprintf(status_message, status_message_size, "Flag at index %d is not a string", i);
            *exit_code_out = 1;
            return 0;
        }
        argv[argc++] = flag->valuestring;

        if (strcmp(flag->valuestring, "-I") == 0 && i + 1 < flag_count) {
            cJSON *include_dir = cJSON_GetArrayItem(flags_json, i + 1);
            if (cJSON_IsString(include_dir) && task_include_count < MAX_TASK_FLAGS) {
                argv[argc++] = "-I";
                make_task_include_path(task_dir, include_dir->valuestring,
                    task_include_paths[task_include_count], sizeof(task_include_paths[task_include_count]));
                argv[argc++] = task_include_paths[task_include_count];
                task_include_count++;
            }
        } else if (strncmp(flag->valuestring, "-I", 2) == 0 && flag->valuestring[2] != '\0' &&
            task_include_count < MAX_TASK_FLAGS) {
            make_task_include_path(task_dir, flag->valuestring + 2,
                task_include_paths[task_include_count], sizeof(task_include_paths[task_include_count]));
            argv[argc++] = "-I";
            argv[argc++] = task_include_paths[task_include_count];
            task_include_count++;
        }
    }

    argv[argc++] = "-I";
    argv[argc++] = (char *)task_dir;

    // Add source and object arguments.
    argv[argc++] = "-c";
    argv[argc++] = (char *)local_source;
    argv[argc++] = "-o";
    argv[argc++] = (char *)local_object;
    argv[argc] = NULL;

    /*
     * Create pipe to capture stdout and stderr of child process
     * pipefd[0] = read end
     * pipefd[1] = write end
    */
    int pipefd[2];
    if(pipe(pipefd) < 0){
        snprintf(status_message, status_message_size, "pipe() failed");
        *exit_code_out = 1;
        return 0;
    }

    // Fork and exec the compiler with the provided arguments, then wait for it to finish and capture the exit code.
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(status_message, status_message_size, "fork() failed");
        *exit_code_out = 1;
        return 0;
    }

    // In the child process, replace the image with the compiler driver.
    if (pid == 0) {
        close(pipefd[0]); // Child does not read

         // Child: send both stdout and stderr into the same pipe
        if((dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)){
            close(pipefd[1]);
            _exit(127);
        }

        close(pipefd[1]);
        execvp(compiler_driver, argv);
        _exit(127);
    }

    close(pipefd[1]); // Parent does not write

    read_from_pipe(pipefd[0], compiler_output, compiler_output_size);
    close(pipefd[0]);

    // In the parent process, wait for the child to finish and capture its exit status.
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(status_message, status_message_size, "waitpid() failed");
        *exit_code_out = 1;
        return 0;
    }

    // Check if the compiler exited normally and capture the exit code. Construct a human-readable status message based on the result.
    if (WIFEXITED(status)) {
        *exit_code_out = WEXITSTATUS(status);
        if (*exit_code_out == 0) {
            snprintf(status_message, status_message_size, "Compiled %s -> %s with %s",
                source, object, compiler_driver);
            return 1;
        }

        snprintf(status_message, status_message_size, "%s failed for %s with exit code %d",
            compiler_driver, source, *exit_code_out);
        return 0;
    }

    snprintf(status_message, status_message_size, "%s terminated abnormally for %s", compiler_driver, source);
    *exit_code_out = 1;
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
    cJSON_AddStringToObject(payload, "source", source);
    cJSON_AddStringToObject(payload, "object", object);
    cJSON_AddStringToObject(payload, "status", status);
    cJSON_AddNumberToObject(payload, "exit_code", exit_code);
    cJSON_AddStringToObject(payload, "message", message);
    cJSON_AddStringToObject(payload, "compiler_output", output);
    cJSON_AddBoolToObject(payload, "has_object", has_object);
    cJSON_AddStringToObject(payload, "object_size", object_size_text);

    send_json_with_payload(sock_fd, MSG_TYPE_TASK_RESULT, payload);
    if (has_object) {
        remocom_send_file_stream(sock_fd, local_object);
    }
}

/// @brief Requests one task from coordinator and processes it.
/// @param sock_fd Coordinator socket.
/// @param buffer Shared receive buffer.
/// @return 1 if worker should continue requesting tasks, 0 otherwise.
static int request_and_process_task(int sock_fd, char *buffer) {
    (void)buffer;
    send_json_message(sock_fd, MSG_TYPE_TASK_REQUEST, "requesting work");

    cJSON *msg = remocom_recv_json_message(sock_fd);
    if (msg == NULL) {
        printf("Invalid task response\n");
        return 0;
    }

    cJSON *type = cJSON_GetObjectItem(msg, "type");
    cJSON *payload = cJSON_GetObjectItem(msg, "payload");
    if (!cJSON_IsString(type) || payload == NULL) {
        cJSON_Delete(msg);
        return 0;
    }

    if (strcmp(type->valuestring, MSG_TYPE_NO_TASK) == 0) {
        printf("Coordinator reported no more tasks\n");
        cJSON_Delete(msg);
        return 0;
    }

    if (strcmp(type->valuestring, "task_error") == 0) {
        char *response = cJSON_PrintUnformatted(payload);
        printf("Coordinator could not prepare task: %s\n", response != NULL ? response : "<unprintable>");
        free(response);
        cJSON_Delete(msg);
        return 0;
    }

    // For task assignments, run the compile and report the result back to the coordinator.
    if (strcmp(type->valuestring, MSG_TYPE_TASK_ASSIGNMENT) == 0) {
        char source[512] = {0};
        char object[512] = {0};
        char task_dir[] = "/tmp/remocom-worker-XXXXXX";
        char local_source[512] = {0};
        char local_object[512] = {0};
        char status_message[512] = {0};
        char compiler_output[2048] = {0}; // Could be larger but then a single JSON message could fill up fast. Future problem to handle.
        int exit_code = 1;
        int success = 0;

        cJSON *source_json = cJSON_GetObjectItem(payload, "source");
        cJSON *object_json = cJSON_GetObjectItem(payload, "object");
        cJSON *files_json = cJSON_GetObjectItem(payload, "files");

        if (cJSON_IsString(source_json)) {
            snprintf(source, sizeof(source), "%s", source_json->valuestring);
        }
        if (cJSON_IsString(object_json)) {
            snprintf(object, sizeof(object), "%s", object_json->valuestring);
        }

        if (!cJSON_IsString(source_json)) {
            snprintf(status_message, sizeof(status_message), "Task payload missing source");
            discard_task_files(sock_fd, files_json);
        } else if (mkdtemp(task_dir) == NULL) {
            snprintf(status_message, sizeof(status_message), "mkdtemp() failed");
            discard_task_files(sock_fd, files_json);
        } else {
            snprintf(local_object, sizeof(local_object), "%s/output.o", task_dir);

            if (!receive_task_files(sock_fd, files_json, task_dir, source_json->valuestring,
                local_source, sizeof(local_source), status_message, sizeof(status_message))) {
                success = 0;
            } else {
                success = run_compile_task(payload, local_source, local_object, task_dir, source, sizeof(source), object, sizeof(object),
                    status_message, sizeof(status_message), compiler_output, sizeof(compiler_output), &exit_code);
            }
        }

        printf("Task completed: %s\n", status_message);
        send_task_result(sock_fd, source, object, success ? "success" : "failure", exit_code,
            status_message, compiler_output, local_object, success);
        cleanup_tree(task_dir);
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
        send_json_message(sock_fd, "heartbeat", "alive");
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
