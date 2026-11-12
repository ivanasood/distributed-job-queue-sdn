
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>


/* UDP heartbeat thread */

typedef struct {
    int worker_id;
    char manager_ip[INET_ADDRSTRLEN];
} heartbeat_args_t;

void *heartbeat_thread(void *arg)
{
    heartbeat_args_t *args = (heartbeat_args_t *)arg;
    int worker_id = args->worker_id;
    char manager_ip[INET_ADDRSTRLEN];

    snprintf(manager_ip, sizeof(manager_ip), "%s", args->manager_ip);
    free(args);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("Heartbeat UDP socket failed");
        return NULL;
    }

    struct sockaddr_in manager_addr = {0};
    manager_addr.sin_family = AF_INET;
    manager_addr.sin_port = htons(8081);

    if (inet_pton(AF_INET, manager_ip, &manager_addr.sin_addr) != 1 ||
        connect(udp_fd, (struct sockaddr *)&manager_addr,
                sizeof(manager_addr)) < 0) {
        perror("Heartbeat UDP connection failed");
        close(udp_fd);
        return NULL;
    }

    struct timeval timeout = {1, 0};
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout));

    char message[128];
    char response[128];

    while (1) {
        snprintf(message, sizeof(message), "HEARTBEAT|%d", worker_id);

        if (send(udp_fd, message, strlen(message), 0) >= 0) {
            ssize_t n = recv(udp_fd, response, sizeof(response) - 1, 0);

            if (n > 0) {
                response[n] = '\0';
                printf("Worker %d UDP response: %s\n",
                       worker_id, response);
            } else {
                printf("Worker %d: UDP heartbeat acknowledgement missing\n",
                       worker_id);
            }
        }

        sleep(3);
    }

    close(udp_fd);
    return NULL;
}

/* Convert the received job type */
int get_job_type(char *type)
{
    if (strcmp(type, "SUM") == 0)
        return JOB_SUM;

    if (strcmp(type, "FACTORIAL") == 0)
        return JOB_FACTORIAL;

    if (strcmp(type, "PRIME") == 0)
        return JOB_PRIME;

    return -1;
}

int main(int argc, char *argv[])
{
    int socket_fd;
    int worker_id;
    struct sockaddr_in server_addr;
    char buffer[MAX_MESSAGE_SIZE];
    char response[MAX_MESSAGE_SIZE];

    if (argc != 2) {
        printf("Usage: ./worker_program <worker_id>\n");
        return 1;
    }

    worker_id = atoi(argv[1]);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    const char *manager_ip = getenv("MANAGER_IP");
    if (manager_ip == NULL)
    	manager_ip = "127.0.0.1";

    inet_pton(AF_INET, manager_ip, &server_addr.sin_addr);

    if (connect(socket_fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(socket_fd);
        return 1;
    }

    printf("Connected to Job Manager\n");

    snprintf(buffer, sizeof(buffer), "REGISTER|%d", worker_id);

    if (send_message(socket_fd, buffer) < 0) {
        printf("Worker registration failed\n");
        close(socket_fd);
        return 1;
    }

    if (receive_message(socket_fd, buffer, sizeof(buffer)) < 0) {
        printf("No registration response received\n");
        close(socket_fd);
        return 1;
    }

    printf("Manager response: %s\n", buffer);

    if (strncmp(buffer, "REGISTERED|", 11) != 0) {
        printf("Worker registration rejected\n");
        close(socket_fd);
        return 1;
    }

    printf("Worker %d registered successfully\n", worker_id);


    heartbeat_args_t *heartbeat_args = malloc(sizeof(*heartbeat_args));

    if (heartbeat_args != NULL) {
        heartbeat_args->worker_id = worker_id;
        snprintf(heartbeat_args->manager_ip,
                 sizeof(heartbeat_args->manager_ip), "%s", manager_ip);

        pthread_t heartbeat_tid;

        if (pthread_create(&heartbeat_tid, NULL,
                           heartbeat_thread, heartbeat_args) == 0) {
            pthread_detach(heartbeat_tid);
        } else {
            perror("Heartbeat thread creation failed");
            free(heartbeat_args);
        }
    } else {
        perror("Heartbeat memory allocation failed");
    }

    /* Continuously receive and execute jobs */
    while (1) {

        if (receive_message(socket_fd, buffer, sizeof(buffer)) < 0) {
            printf("Manager disconnected\n");
            break;
        }

        int id, first, second;
        char type[16];

        if (sscanf(buffer, "JOB|%d|%15[^|]|%d|%d",
                   &id, type, &first, &second) != 4) {
            printf("Invalid job received\n");
            continue;
        }

        job_t job = {0};

        job.job_id = id;
        job.input1 = first;
        job.input2 = second;
        job.status = JOB_RUNNING;

        int job_type = get_job_type(type);

        if (job_type == -1) {
            job.status = JOB_FAILED;
        }
        else {
            job.type = (job_type_t)job_type;

            printf("Worker %d executing Job %d\n",
                   worker_id, id);

            execute_job(&job);
        }

        if (job.status == JOB_COMPLETED) {

            snprintf(response, sizeof(response),
                     "RESULT|%d|COMPLETED|%lld",
                     id, job.result);

        }
        else {

            snprintf(response, sizeof(response),
                     "RESULT|%d|FAILED", id);
        }

        send_message(socket_fd, response);

        printf("Job %d finished. Result: %s\n",
               id, response);
    }

    close(socket_fd);

    return 0;
}
