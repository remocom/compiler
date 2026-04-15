#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions
#include <pthread.h>    // thread library for handling multiple workers
#include <cjson/cJSON.h> // Ultra lightweight JSON parser library
#include <time.h>
#include <stdarg.h>

#define PORT 5000
#define BUFFER_SIZE 1024
#define MAX_WORKERS 100
#define MAX_TASKS 10

/// @brief Represents the status of a worker node, which can be either dead or alive.
typedef enum { dead, alive } NodeStatus;

/// @brief Represents a worker node in the system, containing its ID, socket, IP address, last heartbeat timestamp, and status (alive or dead).
typedef struct {
    int nodeID;
    int socketID;
    char ip_address[INET_ADDRSTRLEN];
    time_t last_heartbeat; // timestamp
    NodeStatus status; // alive or dead
} Node;

Node workers[MAX_WORKERS];
int worker_count = 0;
FILE *log_file;

char *task_queue[MAX_TASKS] = { //list of tasks the coordinator can hand out (tasks still need to be implemented)
    "compile main.c",
    "compile util.c",
    "compile helper.c"
};

int total_tasks = 3; //right now total_tasks is set to 3 
int next_task_index = 0; 

pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER; 
pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER; 

void send_json_message(int client_fd, const char *type_str, const char *payload_str);
void *handle_worker(void *arg);

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
        char *task = task_queue[next_task_index];
        next_task_index++;
        pthread_mutex_unlock(&task_mutex);

        send_json_message(client_fd, "task_assignment", task);
        log_event("TASK ASSIGNED | Node %d | Task: %s\n", node_id, task);
        return;
    }

    pthread_mutex_unlock(&task_mutex);
    send_json_message(client_fd, "no_task", "No tasks available");
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

    if (status == dead) {
        pthread_mutex_unlock(&workers_mutex);

        printf("Received Type: %s | Payload: %s (node already timed out) \n",
            type->valuestring, payload->valuestring);
        log_event("MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s (node already timed out)\n",
            node_id, type->valuestring, payload->valuestring);

        *is_dead = 1;
        return;
    }

    printf("Received Type: %s | Payload: %s \n", type->valuestring, payload->valuestring);
    log_event("MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s\n",
        node_id, type->valuestring, payload->valuestring);

    // Handle heartbeat immediately to keep worker alive in the system.
    if (strcmp(type->valuestring, "heartbeat") == 0) {
        worker->last_heartbeat = time(NULL);
        pthread_mutex_unlock(&workers_mutex);
        return;
    }

    pthread_mutex_unlock(&workers_mutex);

    // Handle other message types outside the lock.
    if (strcmp(type->valuestring, "register") == 0) {
        send_json_message(client_fd, "ack", "Worker registered"); // Acknowledge worker registration.
    } else if (strcmp(type->valuestring, "task_request") == 0) {
        assign_task_to_worker(client_fd, node_id); // Assign a task to this worker if available.
    } else {
        send_json_message(client_fd, "unknown", "Unknown message type"); // Unknown message type received - log and ignore.
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
void *monitor_workers(void *arg){
    (void)arg;

    while(1){
        sleep(10);
        time_t current_time = time(NULL);
        
        pthread_mutex_lock(&workers_mutex); // Lock worker list.

        // Loop through all live workers and check for timeouts.
        for(int i = 0; i < worker_count; i++){
            Node *worker = &workers[i];
            int time_since_heartbeat = current_time - worker->last_heartbeat;

            if(time_since_heartbeat > 15 && worker->status != dead){
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

/// @brief Sends a JSON message to a connected worker.
/// @param client_fd The socket file descriptor of the worker to send the message to.
/// @param type_str The type of the JSON message.
/// @param payload_str The payload of the JSON message.
void send_json_message(int client_fd, const char *type_str, const char *payload_str) {
    // Construct the cJSON object.
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type_str);
    cJSON_AddStringToObject(msg, "payload", payload_str);

    // Convert the cJSON object to a string and send it to the worker.
    char *json_string = cJSON_PrintUnformatted(msg);
    send(client_fd, json_string, strlen(json_string), 0);

    // Deallocate memory used by cJSON object and string.
    free(json_string);
    cJSON_Delete(msg);
}

/// @brief Handles communication with a connected worker node, processing incoming JSON messages, updating worker status based on heartbeats, assigning tasks, and logging events. This function runs in a separate thread for each worker connection.
/// @param arg A pointer to the client socket file descriptor.
/// @return NULL when the worker disconnects or an error occurs.
void *handle_worker(void *arg) {
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
        if (type == NULL || payload == NULL || !cJSON_IsString(type) || !cJSON_IsString(payload)) {
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

/// @brief Initializes the coordinator server, sets up the listening socket, and handles incoming worker connections by spawning a new thread for each worker. It also starts a monitoring thread to check for worker heartbeats and logs all significant events to a log file.
int main() {
    int server_fd;   // server socket (server_fd = phone sitting on desk)
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Open log file for writing coordinator events and errors.
    log_file = fopen("coordinator.log", "w");
    if(log_file == NULL){
        perror("Failed to open log file");
        exit(1);
    }

    server_fd = create_server_socket();

    // Create thread that will monitor heartbeat status of nodes.
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitor_workers, NULL);
    pthread_detach(monitor_thread);

    printf("Coordinator listening on port %d...\n", PORT);

    // Keep the coordinator running so workers can connect (keeps server alive forever. without this it would only accept one connection and exit)
    while (1) {
        int *client_fd_ptr = malloc(sizeof(int)); //allocate memory so each thread gets its own copy of the socket
        if (client_fd_ptr == NULL) {
            perror("Malloc failed");
            continue;
        }

        // Wait here until a worker connects
        *client_fd_ptr = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len); //accept() = someone calls you
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
