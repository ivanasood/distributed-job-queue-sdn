
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void)
{
    const char *manager_ip = getenv("MANAGER_IP");
    if (manager_ip == NULL) manager_ip = "127.0.0.1";

    char messages[4][MAX_MESSAGE_SIZE];
    char buffer[MAX_MESSAGE_SIZE];

    int client_id = getpid();

    snprintf(messages[0], MAX_MESSAGE_SIZE,
             "SUBMIT|%d|SUM|1|100", client_id * 10 + 1);

    snprintf(messages[1], MAX_MESSAGE_SIZE,
             "SUBMIT|%d|FACTORIAL|5|0", client_id * 10 + 2);

    snprintf(messages[2], MAX_MESSAGE_SIZE,
             "SUBMIT|%d|PRIME|17|0", client_id * 10 + 3);

    snprintf(messages[3], MAX_MESSAGE_SIZE,
             "SUBMIT|%d|SUM|1|50", client_id * 10 + 4);

    for (int i = 0; i < 4; i++) {

        int socket_fd;
        struct sockaddr_in server_addr;

        socket_fd = socket(AF_INET, SOCK_STREAM, 0);

        if (socket_fd < 0) {
            perror("Socket creation failed");
            return 1;
        }

        memset(&server_addr, 0, sizeof(server_addr));

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);

        if (inet_pton(AF_INET, manager_ip, &server_addr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid manager IP: %s\n", manager_ip);
            close(socket_fd);
            return 1;
        }

        if (connect(socket_fd,
                    (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) < 0) {

            perror("Connection failed");
            close(socket_fd);
            return 1;
        }

        printf("\nConnected to Job Manager\n");
        printf("Submitting: %s\n", messages[i]);

        if (send_message(socket_fd, messages[i]) < 0) {
            perror("Message sending failed");
            close(socket_fd);
            return 1;
        }

        if (receive_message(socket_fd, buffer,
                            sizeof(buffer)) >= 0) {

            printf("Server response: %s\n", buffer);

        } else {
            printf("Failed to receive response\n");
        }

        close(socket_fd);
    }

    return 0;
}
