#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions
#include <pthread.h>    // thread library for handling multiple workers
#include <cjson/cJSON.h> // Ultra lightweight JSON parser library
#include <time.h>
#include <stdarg.h>
#include "../common/common.h"
#include "manifest_loader.h"

#define PORT 5000
#define BUFFER_SIZE 4096
#define MAX_WORKERS 100
#define MAX_TASKS REMOCOM_MAX_SOURCES
#define MAX_MANIFEST_VALUE REMOCOM_MAX_MANIFEST_VALUE
#define MAX_FLAGS REMOCOM_MAX_FLAGS

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
} Node;

/// @brief Represents a compile task derived from the manifest, containing source/object paths, build output, and flags.
typedef struct {
    char source_path[MAX_MANIFEST_VALUE];
    char object_path[MAX_MANIFEST_VALUE];
    char build_output[MAX_MANIFEST_VALUE];
    char flags[MAX_FLAGS][MAX_MANIFEST_VALUE];
    int flag_count;
} CompileTask;

static Node workers[MAX_WORKERS];
static int worker_count = 0;
static FILE *log_file;

static CompileTask task_queue[MAX_TASKS];
static int total_tasks = 0;
static int next_task_index = 0;
static BuildManifest build_manifest;

static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;

static void send_json_with_payload(int client_fd, const char *type_str, cJSON *payload);
static void send_json_message(int client_fd, const char *type_str, const char *payload_str);
static void *handle_worker(void *arg);

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
    next_task_index = 0;

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

        // Build JSON payload for task assignment message.
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "source", task.source_path);
        cJSON_AddStringToObject(payload, "object", task.object_path);
        cJSON_AddStringToObject(payload, "output", task.build_output);

        cJSON *flags = cJSON_AddArrayToObject(payload, "flags");
        for (int i = 0; i < task.flag_count; i++) {
            cJSON_AddItemToArray(flags, cJSON_CreateString(task.flags[i]));
        }

        send_json_with_payload(client_fd, MSG_TYPE_TASK_ASSIGNMENT, payload);
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
static void handle_task_result(int node_id, cJSON *payload) {
    if (!cJSON_IsObject(payload)) {
        log_event("TASK RESULT | Node %d | Invalid payload\n", node_id);
        return;
    }

    // Extract fields from payload with validation.
    // If any field is missing or of the wrong type, log the issue but attempt to print whatever information is available.
    cJSON *source = cJSON_GetObjectItem(payload, "source");
    cJSON *object = cJSON_GetObjectItem(payload, "object");
    cJSON *status = cJSON_GetObjectItem(payload, "status");
    cJSON *exit_code = cJSON_GetObjectItem(payload, "exit_code");
    cJSON *message = cJSON_GetObjectItem(payload, "message");
    cJSON *output = cJSON_GetObjectItem(payload, "compiler_output");

    const char *source_str = cJSON_IsString(source) ? source->valuestring : "<unknown>";
    const char *object_str = cJSON_IsString(object) ? object->valuestring : "<unknown>";
    const char *status_str = cJSON_IsString(status) ? status->valuestring : "<unknown>";
    int exit_code_val = cJSON_IsNumber(exit_code) ? exit_code->valueint : -1;
    const char *message_str = cJSON_IsString(message) ? message->valuestring : "<none>";

    log_event(
        "TASK RESULT | Node %d | source=%s | object=%s | status=%s | exit_code=%d | message=%s\n",
        node_id, source_str, object_str, status_str, exit_code_val, message_str);

    if(cJSON_IsString(output) && output->valuestring[0] != '\0'){
        log_event("\n\n--- compiler output (Node %d, %s) ---\n%s\n--- end compiler output ---\n\n",
            node_id, source_str, output->valuestring);
        printf("\n\n--- compiler output (Node %d, source=%s, status=%s, exit_code=%d) ---\n%s--- end compiler output ---\n\n",
            node_id, source_str, status_str, exit_code_val, output->valuestring);
    }
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
        handle_task_result(node_id, payload); // Log the task result reported by the worker.
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
            } else {
                log_event("DISCONNECT Node %d | IP: %s | Socket: %d\n",
                    worker->nodeID, worker->ip_address, worker->socketID);
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
            }
        }
        pthread_mutex_unlock(&workers_mutex); // Unlock worker list.
    }

    return NULL;
}

/// @brief Sends a JSON message to a connected worker with any cJSON payload type.
/// @param client_fd The socket file descriptor of the worker to send the message to.
/// @param type_str The type of the JSON message.
/// @param payload The payload object/value for the JSON message.
static void send_json_with_payload(int client_fd, const char *type_str, cJSON *payload) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type_str);
    cJSON_AddItemToObject(msg, "payload", payload);

    // Convert the cJSON object to a string and send it to the worker.
    char *json_string = cJSON_PrintUnformatted(msg);
    send(client_fd, json_string, strlen(json_string), 0);

    // Deallocate memory used by cJSON object and string.
    free(json_string);
    cJSON_Delete(msg);
}

/// @brief Sends a JSON message to a connected worker.
/// @param client_fd The socket file descriptor of the worker to send the message to.
/// @param type_str The type of the JSON message.
/// @param payload_str The payload of the JSON message.
static void send_json_message(int client_fd, const char *type_str, const char *payload_str) {
    cJSON *payload = cJSON_CreateString(payload_str != NULL ? payload_str : "");
    send_json_with_payload(client_fd, type_str, payload);
}

/// @brief Handles communication with a connected worker node, processing incoming JSON messages, updating worker status based on heartbeats, assigning tasks, and logging events. This function runs in a separate thread for each worker connection.
/// @param arg A pointer to the client socket file descriptor.
/// @return NULL when the worker disconnects or an error occurs.
static void *handle_worker(void *arg) {
    int is_dead = 0;
    int client_fd = *(int *)arg; //get the client socket passed from main thread
    free(arg); // free memory after grabbing value

    char buffer[BUFFER_SIZE];
    printf("Helper handling worker\n");

    int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); //recv() == you listen / receive data
    while (bytes > 0) { // connection stays alive as long as bytes are being read
        buffer[bytes] = '\0';   // add string ending character to make sure its printable in C
        cJSON *msg = cJSON_Parse(buffer);

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

        bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    }

    if (bytes == 0) {
        printf("Worker disconnected\n");
    } else if (!is_dead) {
        perror("Receive failed");
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

    log_event("MANIFEST LOADED | output=%s | sources=%d | flags=%d\n",
        build_manifest.output, build_manifest.source_count, build_manifest.flag_count);

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
