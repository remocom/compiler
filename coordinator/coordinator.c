#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions
#include <pthread.h>    // thread library for handling multiple workers
#include <cjson/cJSON.h> // Ultra lightweight JSON parser library
#include <time.h>

#define PORT 5000
#define BUFFER_SIZE 1024
#define MAX_WORKERS 100
#define MAX_TASKS 10

typedef struct {
    int nodeID;
    int socketID;
    char ip_address[INET_ADDRSTRLEN];
    time_t last_heartbeat; // timestamp
    int dead; // 0 = alive, 1 = dead.
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

/*
 * this function monitors each worker's heartbeat every ten seconds
 * and remove a node that hasn't pulsed for 15 seconds or longer.
*/
void *monitor_workers(void *arg){
    while(1){
        sleep(10);
        time_t now = time(NULL);
        pthread_mutex_lock(&workers_mutex);
        for(int i = 0; i < worker_count; i++){
            if(now - workers[i].last_heartbeat > 15 && workers[i].dead != 1){
                pthread_mutex_lock(&log_mutex);
                fprintf(log_file, "[TIMEOUT] Node %d | IP: %s\n", workers[i].nodeID, workers[i].ip_address);
                fflush(log_file);
                pthread_mutex_unlock(&log_mutex);
                workers[i].dead = 1;
                close(workers[i].socketID);
            }
        }
        pthread_mutex_unlock(&workers_mutex);
    }
    return NULL;
}

void send_json_message(int client_fd, const char *type_str, const char *payload_str) { //helper that builds a JSON message and sends it to worker
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type_str);
    cJSON_AddStringToObject(msg, "payload", payload_str);

    char *json_string = cJSON_PrintUnformatted(msg);
    send(client_fd, json_string, strlen(json_string), 0);

    free(json_string);
    cJSON_Delete(msg);
}

// this function is the "helper" that handles each worker (this is like handing the call to another person)
void *handle_worker(void *arg) {
    int isDead = 0;
    int client_fd = *(int *)arg; //get the client socket passed from main thread
    free(arg); //free memory after grabbing value

    char buffer[BUFFER_SIZE];

    printf("Helper handling worker\n");

    // Read the JSON message sent by the worker and parse 
    int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); //recv() == you listen / receive data
    while (bytes > 0) { // connection stays alive as long as bytes are being read
        buffer[bytes] = '\0';   // add string ending character to make sure its printable in C
        cJSON *msg = cJSON_Parse(buffer);
        if(msg == NULL){
            printf("Invalid JSON received\n");
            pthread_mutex_lock(&log_mutex);
            fprintf(log_file, "[WARNING] Invalid JSON received\n");
            fflush(log_file);
            pthread_mutex_unlock(&log_mutex);
            close(client_fd);
            return NULL;
        }
        
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        cJSON *payload = cJSON_GetObjectItem(msg, "payload");
        if(type == NULL || payload == NULL){
            printf("Missing fields in JSON\n");
            pthread_mutex_lock(&log_mutex);
            fprintf(log_file, "[WARNING] Missing fields in JSON message\n");
            fflush(log_file);
            pthread_mutex_unlock(&log_mutex);
            cJSON_Delete(msg);
            close(client_fd);
            return NULL;

        }

        for (int i = 0; i < worker_count; i++) {
            if (workers[i].socketID == client_fd) {
                if(workers[i].dead == 1){ // if node has no heartbeat, then any lingering payload message in pipeline is handled
                    printf("Received Type: %s | Payload: %s (node already timed out) \n",
                        type->valuestring, payload->valuestring);
                    pthread_mutex_lock(&log_mutex);
                    fprintf(log_file, "MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s (node already timed out)\n",
                        workers[i].nodeID, type->valuestring, payload->valuestring);
                    fflush(log_file);
                    pthread_mutex_unlock(&log_mutex);
                    isDead = 1;
                    break;
                }

                printf("Received Type: %s | Payload: %s \n", type->valuestring, payload->valuestring);
                pthread_mutex_lock(&log_mutex);
                fprintf(log_file, "MESSAGE RECEIVED by Node %d | Type: %s | Payload: %s\n",
                    workers[i].nodeID, type->valuestring, payload->valuestring);
                fflush(log_file);
                pthread_mutex_unlock(&log_mutex);

                if(strcmp(type->valuestring, "heartbeat") == 0){
                    pthread_mutex_lock(&workers_mutex);
                    workers[i].last_heartbeat = time(NULL); // update with latest 
                    pthread_mutex_unlock(&workers_mutex);
                } 
                else if(strcmp(type->valuestring, "register") == 0){
                    send_json_message(client_fd, "ack", "Worker registered");
                }
                else if(strcmp(type->valuestring, "task_request") == 0){

                    pthread_mutex_lock(&task_mutex);

                    if(next_task_index < total_tasks){
                        char *task = task_queue[next_task_index];
                        next_task_index++;

                        pthread_mutex_unlock(&task_mutex);

                        send_json_message(client_fd, "task_assignment", task);

                        pthread_mutex_lock(&log_mutex);
                        fprintf(log_file, "TASK ASSIGNED | Node %d | Task: %s\n",
                                workers[i].nodeID, task);
                        fflush(log_file);
                        pthread_mutex_unlock(&log_mutex);
                    }
                    else{
                        pthread_mutex_unlock(&task_mutex);

                        send_json_message(client_fd, "no_task", "No tasks available");
                    }
                }
                else{
                    send_json_message(client_fd, "unknown", "Unknown message type");
                }
                break;
            }
        }
        cJSON_Delete(msg);
        if(isDead){
            break;
        }
        bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    } 
    if (bytes == 0) {
        printf("Worker disconnected\n");
    } else if (!isDead) {
        perror("Receive failed"); // only print if not a timeout
    }

    pthread_mutex_lock(&workers_mutex);

    // find and remove worker
    for (int i = 0; i < worker_count; i++) {
        if (workers[i].socketID == client_fd) {
            pthread_mutex_lock(&log_mutex);
            if(workers[i].dead){
                fprintf(log_file, "REMOVED (timeout) Node %d | IP: %s\n",
                workers[i].nodeID, workers[i].ip_address);
            } else{
                fprintf(log_file, "DISCONNECT Node %d | IP: %s | Socket: %d\n", workers[i].nodeID, 
                    workers[i].ip_address, workers[i].socketID);
            }
            fflush(log_file);
            pthread_mutex_unlock(&log_mutex);
            workers[i] = workers[worker_count - 1];
            worker_count--;
            break;
        }
    }

    printf("Worker removed. Total workers: %d\n", worker_count);

    pthread_mutex_unlock(&workers_mutex);

    // Close this worker connection, but keep server alive (hanging up the phone)
    close(client_fd); //close() == you hang up

    printf("Helper finished worker\n");

    return NULL;
}

int main() {
    int server_fd;   // server socket (server_Fd = phone sitting on desk)
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    log_file = fopen("coordinator.log", "w");
    if(log_file == NULL){
        perror("Failed to open log file");
        exit(1);
    }

    // Create a TCP socket for the coordinator
    server_fd = socket(AF_INET, SOCK_STREAM, 0); //this creates the server or "phone" by using AF_INET (IPv4) and Sock_Stream (TCP)
    if (server_fd < 0) {
        perror("Socket failed");
        exit(1);
    }

    // Set up the server address info (defines where the server lives)
    server_addr.sin_family = AF_INET;              // use IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;      // accept connections on this machine
    server_addr.sin_port = htons(PORT);            // use port 5000

    //port == a "channel" people connect to

    // Attach the socket to the chosen port
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { //bind is saying "hey OS, i want to use port 5000".
        perror("Bind failed");
        exit(1);
    }

    // Put the socket into listening mode
    if (listen(server_fd, 5) < 0) {  //listen means "im ready for incoming connections" and the 5 means 5 people can wait in line
        perror("Listen failed");
        exit(1);
    }

    // Create thread that will monitor heartbeat status of nodes
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

        pthread_mutex_lock(&workers_mutex);
        if (worker_count < MAX_WORKERS) {
            workers[worker_count].socketID = *client_fd_ptr;
            workers[worker_count].nodeID = worker_count;
            workers[worker_count].last_heartbeat = time(NULL);
            workers[worker_count].dead = 0;
            printf("Worker added. Total workers: %d\n", worker_count+1);
            /*Convert raw bytes to readable IP address*/
            inet_ntop(AF_INET, &client_addr.sin_addr, workers[worker_count].ip_address, INET_ADDRSTRLEN);
            pthread_mutex_lock(&log_mutex);
            fprintf(log_file, "CONNECT Node %d | IP: %s | Socket: %d\n", workers[worker_count].nodeID, 
                                    workers[worker_count].ip_address, workers[worker_count].socketID);
            fflush(log_file);
            pthread_mutex_unlock(&log_mutex);
            worker_count++;
        }

        pthread_mutex_unlock(&workers_mutex);

        //after accept you now have two sockets Server_fd which keeps listening and client_fd which handles this connection 
        //exmaple of this is one phone stays on desk (server_fd) and one phone is in your hand (client_fd)

        printf("Worker connected - transferring to helper\n");

        pthread_t thread_id;

        //create a new thread (helper) to handle this worker so the main server can go back to answering calls
        if (pthread_create(&thread_id, NULL, handle_worker, client_fd_ptr) != 0) {
            perror("Thread creation failed");

            pthread_mutex_lock(&workers_mutex);
            for (int i = 0; i < worker_count; i++) {
                if (workers[i].socketID == *client_fd_ptr) {
                    pthread_mutex_lock(&log_mutex);
                    fprintf(log_file, "[ERROR] Failed to connect Node %d to thread.\n", workers[i].nodeID);
                    fflush(log_file);
                    pthread_mutex_unlock(&log_mutex);
                    workers[i] = workers[worker_count - 1];
                    worker_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&workers_mutex);

            close(*client_fd_ptr);
            free(client_fd_ptr);
            continue;
        }

        pthread_detach(thread_id); //let thread clean itself up after finishing so we dont have to track it

        //main thread immediately goes back to accept() to wait for the next worker
        printf("Main ready for next worker\n");
    }

    close(server_fd);
    return 0;
}