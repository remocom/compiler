#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include "../common/common.h"

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

    close(sock_fd);
    return 0;
}