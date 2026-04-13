#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>

#define PORT 5000
#define BUFFER_SIZE 1024

int main() {
    int sock_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket failed");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connect failed");
        exit(1);
    }

    printf("Connected to coordinator\n");

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "register");
    cJSON_AddStringToObject(msg, "payload", "Hello from worker");
    char *json_string = cJSON_Print(msg);
    send(sock_fd, json_string, strlen(json_string), 0);
    cJSON_Delete(msg);
    free(json_string);

    int bytes = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        printf("Received from coordinator: %s\n", buffer);
    }
    
    //Task Request Logic: After registering with the coordinator, the worker sends a "task_request"
    //message to request work. It then waits for a response from the coordinator,
    //which will either be a task assignment or a no-task message.

    cJSON *task_request = cJSON_CreateObject(); //makes a new empty JSON object in memory ({})
    cJSON_AddStringToObject(task_request, "type", "task_request"); // { "type": "task_request"} 
    cJSON_AddStringToObject(task_request, "payload", "requesting work"); // { "type": "task_request", "payload": "requesting work"} 
    char *task_string = cJSON_Print(task_request); //turns JSON object into actual string so it can be sent through the socket
    send(sock_fd, task_string, strlen(task_string), 0); //sends now JSON string over socket to coordinator
    cJSON_Delete(task_request); //frees JSON object memory
    free(task_string); //frees memory created by CJSON_Print()

    bytes = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0); //waits for coordinator to send something back
    if (bytes > 0) {
        buffer[bytes] = '\0'; //adds null term
        printf("Task response from coordinator: %s\n", buffer); // coordinator response
    }
    
    while(1){
        sleep(5); // every 5 seconds send heartbeat
        cJSON *heartbeat = cJSON_CreateObject();
        cJSON_AddStringToObject(heartbeat, "type", "heartbeat");
        cJSON_AddStringToObject(heartbeat, "payload", "alive");
        char *hb_string = cJSON_Print(heartbeat);
        int result = send(sock_fd, hb_string, strlen(hb_string), 0);
        cJSON_Delete(heartbeat);
        free(hb_string);
        
        if(result < 0){
            printf("Coordinator disconnected\n");
            break;
        }
        printf("Heartbeat sent\n");
    }

    close(sock_fd);
    return 0;
}