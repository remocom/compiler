#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>

#define PORT 5000
#define BUFFER_SIZE 1024

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

/// @brief Sends a JSON message to the coordinator.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param type The type of the message.
/// @param payload The payload of the message.
static void send_json_message(int sock_fd, const char *type, const char *payload) {
    cJSON *msg = cJSON_CreateObject(); // makes a new empty JSON object in memory ({})
    cJSON_AddStringToObject(msg, "type", type);       // add "type" field ({type: "some type"})
    cJSON_AddStringToObject(msg, "payload", payload); // add "payload" field ({type: "some type", payload: "some payload"})

    // turns JSON object into actual string so it can be sent through the socket
    char *json_string = cJSON_Print(msg);
    send(sock_fd, json_string, strlen(json_string), 0); // sends JSON string over socket to coordinator

    cJSON_Delete(msg);   // frees JSON object memory
    free(json_string);   // frees memory created by cJSON_Print()
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

/// @brief Requests a task from the coordinator and waits for a response.
/// @param sock_fd The file descriptor for the coordinator socket.
/// @param buffer The buffer to receive data into.
static void request_task(int sock_fd, char *buffer) {
    // Task Request Logic: After registering with the coordinator, the worker sends a "task_request"
    // message to request work. It then waits for a response from the coordinator,
    // which will either be a task assignment or a no-task message.
    send_json_message(sock_fd, "task_request", "requesting work");

    int bytes = receive_into_buffer(sock_fd, buffer); // waits for coordinator to send something back
    if (bytes > 0) {
        printf("Task response from coordinator: %s\n", buffer); // coordinator response
    }
}

/// @brief Sends heartbeats to the coordinator at regular intervals.
/// @param sock_fd The file descriptor for the coordinator socket.
static void heartbeat_loop(int sock_fd) {
    while (1) {
        sleep(5); // every 5 seconds send heartbeat

        // heartbeat message tells coordinator this worker is still alive
        cJSON *heartbeat = cJSON_CreateObject();
        cJSON_AddStringToObject(heartbeat, "type", "heartbeat");
        cJSON_AddStringToObject(heartbeat, "payload", "alive");

        char *hb_string = cJSON_Print(heartbeat);
        int result = send(sock_fd, hb_string, strlen(hb_string), 0);

        cJSON_Delete(heartbeat);
        free(hb_string);

        if (result < 0) {
            printf("Coordinator disconnected\n");
            break;
        }

        printf("Heartbeat sent\n");
    }
}

/// @brief The main entry point for the worker process.
/// @return The exit status of the program.
int main() {
    char buffer[BUFFER_SIZE];
    int sock_fd = create_worker_socket();

    // connect() == worker dials coordinator
    connect_to_coordinator(sock_fd);

    printf("Connected to coordinator\n");

    // Register with the coordinator, request a task, and start sending heartbeats in a loop.
    register_with_coordinator(sock_fd, buffer);
    request_task(sock_fd, buffer);
    heartbeat_loop(sock_fd);

    close(sock_fd);
    return 0;
}