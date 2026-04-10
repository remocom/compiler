#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit
#include <string.h>     // strlen
#include <unistd.h>     // close
#include <arpa/inet.h>  // socket structs and networking functions

#define PORT 5000
#define BUFFER_SIZE 1024

int main() {
    int server_fd, client_fd;   // server socket and connected client socket (server_Fd = phone sitting on desk  client_fd = active phone call)
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    char buffer[BUFFER_SIZE];

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

        // Wait here until a worker connects
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len); //accept() = someone calls you
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        //after accept you now have two sockets Server_fd which keeps listening and client_fd which handles this connection 
        //exmaple of this is one phone stays on desk (server_fd) and one phone is in your hand (client_fd)

        printf("Worker connected\n");

        // Read the message sent by the worker
        int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0); //recv() == you listen / receive data
        if (bytes > 0) {
            buffer[bytes] = '\0';   // add string ending character to make sure its priintable in C
            printf("Received: %s\n", buffer);
        } else if (bytes == 0) {
            printf("Worker disconnected\n");
        } else {
            perror("Receive failed");
        }

        // Send a reply back to the worker
        char *response = "Hello from coordinator!";
        send(client_fd, response, strlen(response), 0); //send() == you talk back

        // Close this worker connection, but keep server alive (hanging up the phone)
        close(client_fd); //close() == you hang up
    }

    close(server_fd);
    return 0;
}