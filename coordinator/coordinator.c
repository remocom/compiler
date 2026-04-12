#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions
#include <pthread.h>    // thread library for handling multiple workers

#define PORT 5000
#define BUFFER_SIZE 1024
#define MAX_WORKERS 100

typedef struct {
    int nodeID;
    int socketID;
    char ip_address[INET_ADDRSTRLEN];
} Node;

Node workers[MAX_WORKERS];
int worker_count = 0;
FILE *log_file;

pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

//this function is the "helper" that handles each worker (this is like handing the call to another person)
void *handle_worker(void *arg) {
    int client_fd = *(int *)arg; //get the client socket passed from main thread
    free(arg); //free memory after grabbing value

    char buffer[BUFFER_SIZE];

    printf("Helper handling worker\n");

    // Read the message sent by the worker
    int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); //recv() == you listen / receive data
    if (bytes > 0) {
        buffer[bytes] = '\0';   // add string ending character to make sure its printable in C
        printf("Received: %s\n", buffer);
        for (int i = 0; i < worker_count; i++) {
            if (workers[i].socketID == client_fd) {
                pthread_mutex_lock(&log_mutex);
                fprintf(log_file, "MESSAGE RECEIVED by Node %d: %s\n", workers[i].nodeID, buffer);
                pthread_mutex_unlock(&log_mutex);
                fflush(log_file);
                break;
            }
        }    
    } else if (bytes == 0) {
        printf("Worker disconnected\n");
    } else {
        perror("Receive failed");
    }

    sleep(5); //forces each helper to pause for 5 seconds while holding the connection as everything was finishing too fast

    // Send a reply back to the worker
    const char *response = "Hello from coordinator!"; //changed to const since string should not be modified
    send(client_fd, response, strlen(response), 0); //send() == you talk back

    pthread_mutex_lock(&workers_mutex);

// find and remove worker
    for (int i = 0; i < worker_count; i++) {
        if (workers[i].socketID == client_fd) {
            pthread_mutex_lock(&log_mutex);
            fprintf(log_file, "DISCONNECT Node %d | IP: %s | Socket: %d\n", workers[i].nodeID, 
                workers[i].ip_address, workers[i].socketID);
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

    char buffer[BUFFER_SIZE];

    log_file = fopen("coordinator.log", "a");
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