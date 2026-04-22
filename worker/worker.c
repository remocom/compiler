#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cjson/cJSON.h>
#include "../common/common.h"

#define PORT 5000
#define BUFFER_SIZE 4096
#define MAX_TASK_FLAGS 64

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
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type);
    cJSON_AddItemToObject(msg, "payload", payload);

    char *json_string = cJSON_PrintUnformatted(msg);
    send(sock_fd, json_string, strlen(json_string), 0);

    cJSON_Delete(msg);
    free(json_string);
}

/// @brief Sends a JSON message to the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param type The type of the message.
/// @param payload The payload of the message.
static void send_json_message(int sock_fd, const char *type, const char *payload) {
    cJSON *payload_value = cJSON_CreateString(payload != NULL ? payload : "");
    send_json_with_payload(sock_fd, type, payload_value);
}

/// @brief Sends a handshake payload describing worker build/runtime compatibility.
/// @param sock_fd The file descriptor for the coordinator socket.
static void send_handshake_message(int sock_fd) {
    cJSON *msg = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();

    cJSON_AddStringToObject(msg, "type", MSG_TYPE_HANDSHAKE);
    cJSON_AddItemToObject(msg, "payload", payload);

    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_GCC_VERSION, __VERSION__);
    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_TARGET_ARCH, remocom_detect_target_arch());
    cJSON_AddStringToObject(payload, HANDSHAKE_KEY_TARGET_OS, remocom_detect_target_os());
    cJSON_AddNumberToObject(payload, HANDSHAKE_KEY_RPC_PROTOCOL_VERSION, REMOCOM_RPC_PROTOCOL_VERSION);

    // Convert JSON object into a string to be sent through the socket.
    char *json_string = cJSON_PrintUnformatted(msg);
    send(sock_fd, json_string, strlen(json_string), 0);

    cJSON_Delete(msg);
    free(json_string);
}

/// @brief Receives data from the coordinator into a buffer and stores it in the buffer.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer The buffer to receive data into.
/// @return The number of bytes received.
static int receive_into_buffer(int sock_fd, char *buffer) {
    // recv() == listen / receive data from coordinator
    int bytes = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0'; // add string ending character so C prints safely
    }
    return bytes;
}

/// @brief Performs compatibility handshake and returns 1 on success.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer Reusable receive buffer.
/// @return 1 if handshake accepted, 0 otherwise.
static int perform_handshake(int sock_fd, char *buffer) {
    send_handshake_message(sock_fd);

    int bytes = receive_into_buffer(sock_fd, buffer);
    if (bytes <= 0) {
        return 0;
    }

    cJSON *msg = cJSON_Parse(buffer);
    if (msg == NULL) {
        return 0;
    }

    cJSON *type = cJSON_GetObjectItem(msg, "type");
    int accepted = cJSON_IsString(type) && strcmp(type->valuestring, MSG_TYPE_HANDSHAKE_ACK) == 0;

    cJSON_Delete(msg);

    if (!accepted) {
        printf("Handshake failed: %s\n", buffer);
        return 0;
    }

    printf("Handshake accepted by coordinator\n");
    return 1;
}

/// @brief Registers the worker with the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer The buffer to receive data into.
static void register_with_coordinator(int sock_fd, char *buffer) {
    // send initial register message so coordinator knows this worker joined
    send_json_message(sock_fd, "register", "Hello from worker");

    int bytes = receive_into_buffer(sock_fd, buffer);
    if (bytes > 0) {
        printf("Received from coordinator: %s\n", buffer);
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

/// @brief Executes a compile task received from the coordinator by invoking GCC.
/// @param payload Parsed task payload containing source/object/flags.
/// @param source Output buffer for source path.
/// @param source_size Size of source buffer.
/// @param object Output buffer for object path.
/// @param object_size Size of object buffer.
/// @param status_message Output buffer for human-readable result.
/// @param status_message_size Size of status buffer.
/// @param compiler_output Buffer that receives captured stdout/stderr from the compiler process.
/// @param compiler_output_size Size of output buffer.
/// @param exit_code_out Exit code from GCC or local failure.

/// @return 1 on successful compile, 0 on failure.
static int run_compile_task(
    cJSON *payload,
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

    char *argv[MAX_TASK_FLAGS + 7];
    int argc = 0;
    argv[argc++] = "gcc";

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
    }

    // Add source and object arguments.
    argv[argc++] = "-c";
    argv[argc++] = source;
    argv[argc++] = "-o";
    argv[argc++] = object;
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

    // Fork and exec GCC with the provided arguments, then wait for it to finish and capture the exit code.
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(status_message, status_message_size, "fork() failed");
        *exit_code_out = 1;
        return 0;
    }

    // In the child process, replace the image with GCC.
    if (pid == 0) {
        close(pipefd[0]); // Child does not read

         // Child: send both stdout and stderr into the same pipe
        if((dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)){
            close(pipefd[1]);
            _exit(127);
        }

        close(pipefd[1]);
        execvp("gcc", argv);
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

    // Check if GCC exited normally and capture the exit code. Construct a human-readable status message based on the result.
    if (WIFEXITED(status)) {
        *exit_code_out = WEXITSTATUS(status);
        if (*exit_code_out == 0) {
            snprintf(status_message, status_message_size, "Compiled %s -> %s", source, object);
            return 1;
        }

        snprintf(status_message, status_message_size, "gcc failed for %s with exit code %d", source, *exit_code_out);
        return 0;
    }

    snprintf(status_message, status_message_size, "gcc terminated abnormally for %s", source);
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
    int exit_code, const char *message, const char *output) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "source", source);
    cJSON_AddStringToObject(payload, "object", object);
    cJSON_AddStringToObject(payload, "status", status);
    cJSON_AddNumberToObject(payload, "exit_code", exit_code);
    cJSON_AddStringToObject(payload, "message", message);
    cJSON_AddStringToObject(payload, "compiler_output", output);

    send_json_with_payload(sock_fd, MSG_TYPE_TASK_RESULT, payload);
}

/// @brief Requests one task from coordinator and processes it.
/// @param sock_fd Coordinator socket.
/// @param buffer Shared receive buffer.
/// @return 1 if worker should continue requesting tasks, 0 otherwise.
static int request_and_process_task(int sock_fd, char *buffer) {
    send_json_message(sock_fd, MSG_TYPE_TASK_REQUEST, "requesting work");

    int bytes = receive_into_buffer(sock_fd, buffer);
    if (bytes <= 0) {
        return 0;
    }

    cJSON *msg = cJSON_Parse(buffer);
    if (msg == NULL) {
        printf("Invalid task response: %s\n", buffer);
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

    // For task assignments, run the compile and report the result back to the coordinator.
    if (strcmp(type->valuestring, MSG_TYPE_TASK_ASSIGNMENT) == 0) {
        char source[512] = {0};
        char object[512] = {0};
        char status_message[512] = {0};
        char compiler_output[2048] = {0}; // Could be larger but then a single JSON message could fill up fast. Future problem to handle.
        int exit_code = 1;

        int success = run_compile_task(payload, source, sizeof(source), object, sizeof(object),
            status_message, sizeof(status_message), compiler_output, sizeof(compiler_output), &exit_code);

        printf("Task completed: %s\n", status_message);
        send_task_result(sock_fd, source, object, success ? "success" : "failure", exit_code, status_message, compiler_output);
    } else {
        printf("Unexpected response from coordinator: %s\n", buffer);
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