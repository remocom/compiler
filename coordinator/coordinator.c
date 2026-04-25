#include <stdio.h>          // printf, perror
#include <stdlib.h>         // exit
#include <string.h>         // strlen
#include <unistd.h>         // close
#include <arpa/inet.h>      // socket structs and networking functions
#include <pthread.h>        // thread library for handling multiple workers
#include <cjson/cJSON.h>    // Ultra lightweight JSON parser library
#include <stdarg.h>         // va_list and related functions for variable argument logging
#include <errno.h>          // errno for error handling
#include <inttypes.h>       // PRIu64 for portable uint64_t printing
#include <limits.h>         // PATH_MAX for maximum path length

#include "../common/common.h"
#include "build_state.h"
#include "coordinator_types.h"
#include "linker.h"
#include "manifest_loader.h"
#include "task_dispatch.h"
#include "worker_registry.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PORT 5000
#define BUFFER_SIZE 4096

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

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t build_mutex = PTHREAD_MUTEX_INITIALIZER;
static BuildStateContext build_state;
static TaskDispatchContext task_dispatch;
static LinkerContext linker_context;

static void send_json_message(int client_fd, const char *type_str, const char *payload_str);
static void *handle_worker(void *arg);
static void log_task_dispatch_message(void *callback_ctx, const char *message);
static void requeue_worker_task(void *callback_ctx, const CompileTask *task);

/// @brief Changes the current directory to the manifest's directory.
/// @param manifest_path The path to the manifest file.
/// @param error_buf The buffer to store any error messages.
/// @param error_buf_size The size of the error buffer.
/// @return 1 on success, 0 on failure.
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

/// @brief Loads a subset of TOML manifest format for the [build] section.
/// @param manifest_path Path to manifest file.
/// @param manifest Parsed output manifest.
/// @param error_buf Destination buffer for validation errors.
/// @param error_buf_size Size of error buffer.
/// @return 1 on success, 0 on parse/validation failure.
static int load_manifest_file(
    const char *manifest_path, BuildManifest *manifest,
    char *error_buf, size_t error_buf_size
) {
    return remocom_load_manifest_file(manifest_path, manifest, error_buf, error_buf_size);
}

/// @brief Converts manifest sources into the coordinator's task queue.
/// @param manifest Parsed build manifest.
/// @param error_buf Destination buffer for validation errors.
/// @param error_buf_size Size of error buffer.
/// @return 1 on success, 0 when queue/object path validation fails.
static int build_compile_tasks_from_manifest(
    const BuildManifest *manifest,
    char *error_buf, size_t error_buf_size
) {
    total_tasks = 0;
    original_task_count = 0;
    next_task_index = 0;
    object_ready_count = 0;
    build_failed = 0;
    link_started = 0;
    memset(object_ready, 0, sizeof(object_ready));

    // For each source in the manifest, create a compile task with derived object paths and
    // manifest flags. Validate that we don't exceed the maximum task count and that object paths
    // can be derived within buffer limits.
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
/// @param payload Parsed payload object from incoming message.
/// @param reason_buf Output buffer for mismatch reason.
/// @param reason_buf_len Size of reason buffer.
/// @return 1 if payload matches coordinator requirements, 0 otherwise.
static int validate_handshake_payload(cJSON *payload, char *reason_buf, size_t reason_buf_len) {
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

    return 1;
}

/// @brief Thread-safe logging helper for coordinator events.
/// @param format printf-style format string.
static void log_event(const char *format, ...) {
    pthread_mutex_lock(&log_mutex);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

/// @brief Logs messages from task dispatch and linker contexts to the coordinator log file.
/// @param message The message to log.
static void log_task_dispatch_message(void *callback_ctx, const char *message) {
    (void)callback_ctx;
    log_event("%s", message);
}

/// @brief Requeues a worker task for reassignment.
/// @param task The task to requeue.
static void requeue_worker_task(void *callback_ctx, const CompileTask *task) {
    (void)callback_ctx;
    remocom_enqueue_task_for_reassign(&task_dispatch, task);
}

/// @brief Configures the worker registry with logging and requeueing callbacks.
static void configure_worker_registry(void) {
    WorkerRegistryConfig config;
    config.log_message = log_task_dispatch_message;
    config.requeue_task = requeue_worker_task;
    config.callback_ctx = NULL;
    remocom_worker_registry_configure(&config);
}

/// @brief Configures the linker context.
static void configure_linker_context(void) {
    linker_context.manifest = &build_manifest;
    linker_context.tasks = task_queue;
    linker_context.task_count = original_task_count;
    linker_context.log_message = log_task_dispatch_message;
    linker_context.callback_ctx = NULL;
}

/// @brief Configures the build state context with task information and logging callback.
static void configure_build_state_context(void) {
    build_state.tasks = task_queue;
    build_state.task_count = original_task_count;
    build_state.object_ready = object_ready;
    build_state.object_ready_count = &object_ready_count;
    build_state.build_failed = &build_failed;
    build_state.link_started = &link_started;
    build_state.build_mutex = &build_mutex;
    build_state.log_message = log_task_dispatch_message;
    build_state.callback_ctx = NULL;
}

/// @brief Configures the task dispatch context with task queue information and callbacks for
/// logging and marking tasks active.
static void configure_task_dispatch_context(void) {
    task_dispatch.task_queue = task_queue;
    task_dispatch.max_tasks = MAX_TASKS;
    task_dispatch.total_tasks = &total_tasks;
    task_dispatch.next_task_index = &next_task_index;
    task_dispatch.manifest = &build_manifest;
    task_dispatch.task_mutex = &task_mutex;
    task_dispatch.log_message = log_task_dispatch_message;
    task_dispatch.mark_task_active = remocom_worker_registry_mark_task_active;
    task_dispatch.callback_ctx = NULL;
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

    return remocom_record_task_result_for_link(
        &build_state,
        source_str,
        object_str,
        compile_succeeded && object_received
    );
}

/// @brief Handles a parsed worker message after JSON validation.
/// @param client_fd Worker socket.
/// @param type Parsed message type field.
/// @param payload Parsed message payload field.
/// @param is_dead Set to 1 when worker should be disconnected.
static void dispatch_worker_message(int client_fd, cJSON *type, cJSON *payload, int *is_dead) {
    WorkerSnapshot worker;
    if (!remocom_worker_registry_get_snapshot(client_fd, &worker)) {
        return;
    }

    int node_id = worker.node_id;

    char *payload_str = cJSON_PrintUnformatted(payload);
    if (payload_str == NULL) {
        payload_str = strdup("<unprintable>");
    }

    if (worker.status == WORKER_STATUS_DEAD) {
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
        int handshake_ok = validate_handshake_payload(payload, mismatch_reason, sizeof(mismatch_reason));

        if (handshake_ok) {
            remocom_worker_registry_mark_handshake_completed(client_fd);
            send_json_message(client_fd, MSG_TYPE_HANDSHAKE_ACK, "Handshake accepted");
            log_event("HANDSHAKE ACCEPTED Node %d\n", node_id);
            return;
        }

        send_json_message(client_fd, MSG_TYPE_HANDSHAKE_REJECT, mismatch_reason);
        log_event("HANDSHAKE REJECTED Node %d | Reason: %s\n", node_id, mismatch_reason);
        *is_dead = 1;
        return;
    }

    if (!worker.handshake_completed) {
        send_json_message(client_fd, MSG_TYPE_HANDSHAKE_REQUIRED, "Handshake required before registration");
        return;
    }

    // Handle heartbeat immediately to keep worker alive in the system.
    if (strcmp(type->valuestring, "heartbeat") == 0) {
        remocom_worker_registry_update_heartbeat(client_fd);
        return;
    }

    // Handle other message types outside the lock.
    if (strcmp(type->valuestring, "register") == 0) {
        send_json_message(client_fd, "ack", "Worker registered"); // Acknowledge registration.
    } else if (strcmp(type->valuestring, MSG_TYPE_TASK_REQUEST) == 0) {
        remocom_assign_task_to_worker(&task_dispatch, client_fd, node_id);
    } else if (strcmp(type->valuestring, MSG_TYPE_TASK_RESULT) == 0) {
        int should_link = handle_task_result(client_fd, node_id, payload); // Log the task result reported by the worker.

        // Worker finished this task - clear the active-task marker so a later
        // disconnect doesn't trigger a task reassignment.
        remocom_worker_registry_clear_active_task(client_fd);

        if (should_link) {
            remocom_run_link_step(&linker_context);
        }

    } else {
        send_json_message(client_fd, "unknown", "Unknown message type");
    }
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
        remocom_worker_registry_remove_for_thread_failure(*client_fd_ptr);
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

    remocom_worker_registry_remove_by_socket(client_fd);
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
    configure_task_dispatch_context();
    configure_linker_context();
    configure_build_state_context();
    configure_worker_registry();

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
    pthread_create(&monitor_thread, NULL, remocom_worker_registry_monitor, NULL);
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
        if (!remocom_worker_registry_register(*client_fd_ptr, &client_addr)) {
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
