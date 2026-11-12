#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>


extern pthread_mutex_t queue_mutex;

/* Notify SDN controller whenever a worker changes availability. */
int notify_sdn_worker_status(int worker_id, const char *status)
{
    if ((worker_id != 1 && worker_id != 2) ||
        (strcmp(status, "available") != 0 &&
         strcmp(status, "unavailable") != 0))
        return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct timeval timeout = {2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(5000);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("SDN status notification failed: controller unavailable\n");
        close(sock);
        return -1;
    }

    char body[128];
    snprintf(body, sizeof(body),
             "{\"worker\":\"worker%d\",\"status\":\"%s\"}",
             worker_id, status);

    char request[512];
    int length = snprintf(
        request, sizeof(request),
        "POST /worker-status HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        strlen(body), body
    );

    int result = -1;

    if (length > 0 && (size_t)length < sizeof(request)) {
        size_t sent_total = 0;

        while (sent_total < (size_t)length) {
            ssize_t sent = send(sock, request + sent_total,
                                (size_t)length - sent_total, 0);
            if (sent <= 0)
                break;
            sent_total += (size_t)sent;
        }

        if (sent_total == (size_t)length) {
            char response[512] = {0};
            ssize_t received = recv(sock, response,
                                    sizeof(response) - 1, 0);

            if (received > 0 &&
                (strstr(response, "HTTP/1.1 200") ||
                 strstr(response, "HTTP/1.0 200"))) {
                printf("SDN notified: Worker %d -> %s\n",
                       worker_id, status);
                result = 0;
            } else {
                printf("SDN rejected Worker %d status update: %s\n",
                       worker_id, response);
            }
        }
    }

    close(sock);
    return result;
}

/* Information passed to each job execution thread */

typedef struct {
    job_t *job;
    int worker_id;
    int worker_socket;
} worker_task_t;


/* Request a network route from the Python SDN controller.
   Returns 1 only when the controller confirms an available route. */

int request_sdn_route(int worker_id)
{
    int sock;
    struct sockaddr_in address;
    char request[256];
    char response[2048] = {0};
    const char *worker_name;
    size_t total_sent = 0;
    size_t total_received = 0;
    size_t request_length;
    ssize_t received;
    struct timeval timeout;

    if (worker_id == 1)
        worker_name = "worker1";
    else if (worker_id == 2)
        worker_name = "worker2";
    else {
        printf("SDN: Unknown worker ID %d\n", worker_id);
        return 0;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("SDN socket creation failed");
        return 0;
    }

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout));

    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               &timeout, sizeof(timeout));

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(5000);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &address.sin_addr) != 1) {
        close(sock);
        return 0;
    }

    if (connect(sock, (struct sockaddr *)&address,
                sizeof(address)) < 0) {

        printf("SDN controller unavailable. "
               "Job will not be dispatched.\n");

        close(sock);
        return 0;
    }

    snprintf(request, sizeof(request),
             "GET /route?worker=%s HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Connection: close\r\n\r\n",
             worker_name);

    request_length = strlen(request);

    /* Send HTTP request */

    while (total_sent < request_length) {

        ssize_t sent = send(
            sock,
            request + total_sent,
            request_length - total_sent,
            0
        );

        if (sent <= 0) {
            printf("SDN request send failed.\n");
            close(sock);
            return 0;
        }

        total_sent += (size_t)sent;
    }

    /* Receive controller response */

    while (total_received < sizeof(response) - 1) {

        received = recv(
            sock,
            response + total_received,
            sizeof(response) - 1 - total_received,
            0
        );

        if (received <= 0)
            break;

        total_received += (size_t)received;
    }

    response[total_received] = '\0';

    close(sock);

    printf("\n========== SDN ROUTE INFORMATION ==========\n");
    printf("Selected Worker: %d\n%s\n",
           worker_id,
           total_received ? response :
           "No response from SDN controller.");

    printf("============================================\n\n");

    /* Accept only a successful HTTP response containing a route. */

    if ((strstr(response, "HTTP/1.1 200") != NULL ||
         strstr(response, "HTTP/1.0 200") != NULL) &&
        strstr(response, "\"path\"") != NULL) {

        return 1;
    }

    printf("No valid SDN route available for Worker %d.\n",
           worker_id);

    return 0;
}


/* Requeue a job after worker communication failure */

void retry_or_fail(job_t *job)
{
    int should_retry = 0;
    int attempt = 0;

    pthread_mutex_lock(&queue_mutex);

    if (job->retry_count < MAX_JOB_RETRIES) {

        job->retry_count++;
        attempt = job->retry_count;

        job->status = JOB_QUEUED;
        should_retry = 1;
    }
    else {

        job->status = JOB_FAILED;

        pthread_cond_broadcast(&job->completed);
    }

    pthread_mutex_unlock(&queue_mutex);

    if (should_retry) {

        printf("Retrying Job %d (Retry %d of %d)\n",
               job->job_id, attempt, MAX_JOB_RETRIES);

        enqueue_job(job);
    }
    else {

        printf("Job %d failed after maximum retries\n",
               job->job_id);
    }
}


/* Execute one job using an assigned worker */

void *worker_task_thread(void *arg)
{
    worker_task_t *task = (worker_task_t *)arg;

    job_t *job = task->job;

    int worker_id = task->worker_id;
    int worker_socket = task->worker_socket;
    int job_id = job->job_id;

    free(task);

    char message[MAX_MESSAGE_SIZE];
    char response[MAX_MESSAGE_SIZE];

    char *type;

    if (job->type == JOB_SUM)
        type = "SUM";

    else if (job->type == JOB_FACTORIAL)
        type = "FACTORIAL";

    else if (job->type == JOB_PRIME)
        type = "PRIME";

    else
        type = NULL;

    if (type == NULL) {

        record_worker_failure(worker_id);
        remove_worker(worker_id);

        retry_or_fail(job);

        return NULL;
    }

    snprintf(message, sizeof(message),
             "JOB|%d|%s|%d|%d",
             job_id, type, job->input1, job->input2);

    printf("Sending Job %d to Worker %d\n",
           job_id, worker_id);

    /* Send job to worker */

    if (send_message(worker_socket, message) < 0) {

        printf("Failed to send Job %d to Worker %d\n",
               job_id, worker_id);

        record_worker_failure(worker_id);
        remove_worker(worker_id);

        retry_or_fail(job);

        return NULL;
    }

    /* Receive result from worker */

    if (receive_message(worker_socket, response,
                        sizeof(response)) < 0) {

        printf("Worker %d disconnected\n", worker_id);

        record_worker_failure(worker_id);
        remove_worker(worker_id);

        retry_or_fail(job);

        return NULL;
    }

    int result_id;
    int failed_id;

    long long result;

    /* Successful execution */

    if (sscanf(response, "RESULT|%d|COMPLETED|%lld",
               &result_id, &result) == 2 &&
        result_id == job_id) {

        pthread_mutex_lock(&queue_mutex);

        job->result = result;
        job->status = JOB_COMPLETED;

        pthread_cond_broadcast(&job->completed);

        pthread_mutex_unlock(&queue_mutex);

        record_worker_completion(worker_id);

        printf("Job %d completed by Worker %d\n",
               job_id, worker_id);

        release_worker(worker_id);
    }

    /* Worker reports an execution failure */

    else if (sscanf(response, "RESULT|%d|FAILED",
                    &failed_id) == 1 &&
             failed_id == job_id) {

        pthread_mutex_lock(&queue_mutex);

        job->status = JOB_FAILED;

        pthread_cond_broadcast(&job->completed);

        pthread_mutex_unlock(&queue_mutex);

        record_worker_failure(worker_id);

        release_worker(worker_id);

        printf("Job %d failed during execution on Worker %d\n",
               job_id, worker_id);
    }

    /* Invalid response */

    else {

        printf("Invalid response from Worker %d\n",
               worker_id);

        record_worker_failure(worker_id);
        remove_worker(worker_id);

        retry_or_fail(job);
    }

    return NULL;
}


/* Dispatcher thread */

void *scheduler_thread(void *arg)
{
    (void)arg;

    while (1) {

        job_t *job = dequeue_job();

        if (job == NULL)
            continue;

        int job_id = job->job_id;

        int worker_id;
        int worker_socket;

        /* Wait for an available worker */

        while (get_available_worker(&worker_id,
                                    &worker_socket) != 0) {

            sleep(1);
        }

        printf("Assigning Job %d to Worker %d\n",
               job_id, worker_id);

        /* Validate the SDN path before dispatching the job. */

        if (request_sdn_route(worker_id) == 0) {

            printf("SDN path unavailable for Worker %d.\n",
                   worker_id);

            /* Release worker without recording a worker failure. */

            release_worker(worker_id);

            /* Requeue using the existing retry mechanism. */

            retry_or_fail(job);

            continue;
        }

        worker_task_t *task =
            malloc(sizeof(worker_task_t));

        if (task == NULL) {

            release_worker(worker_id);

            pthread_mutex_lock(&queue_mutex);

            job->status = JOB_FAILED;

            pthread_cond_broadcast(&job->completed);

            pthread_mutex_unlock(&queue_mutex);

            continue;
        }

        task->job = job;
        task->worker_id = worker_id;
        task->worker_socket = worker_socket;

        pthread_t worker_thread;

        if (pthread_create(&worker_thread, NULL,
                           worker_task_thread, task) != 0) {

            printf("Thread creation failed for Job %d\n",
                   job_id);

            free(task);

            release_worker(worker_id);

            pthread_mutex_lock(&queue_mutex);

            job->status = JOB_FAILED;

            pthread_cond_broadcast(&job->completed);

            pthread_mutex_unlock(&queue_mutex);

            continue;
        }

        pthread_detach(worker_thread);

        /*
         * Do not wait for this job here.
         * Immediately dispatch the next queued job.
         */
    }

    return NULL;
}


/* Handle client job submissions */

void handle_client(int client_fd, char *buffer)
{
    char response[MAX_MESSAGE_SIZE];

    int id, first, second;
    char type[16];

    if (sscanf(buffer, "SUBMIT|%d|%15[^|]|%d|%d",
               &id, type, &first, &second) != 4) {

        send_message(client_fd, "ERROR|INVALID_REQUEST");

        close(client_fd);
        return;
    }

    job_t *job = malloc(sizeof(job_t));

    if (job == NULL) {

        send_message(client_fd,
                     "ERROR|MEMORY_ALLOCATION_FAILED");

        close(client_fd);
        return;
    }

    job->job_id = id;
    job->input1 = first;
    job->input2 = second;

    job->result = 0;
    job->retry_count = 0;

    job->status = JOB_QUEUED;
    job->next = NULL;

    if (strcmp(type, "SUM") == 0)
        job->type = JOB_SUM;

    else if (strcmp(type, "FACTORIAL") == 0)
        job->type = JOB_FACTORIAL;

    else if (strcmp(type, "PRIME") == 0)
        job->type = JOB_PRIME;

    else {

        free(job);

        send_message(client_fd, "ERROR|UNKNOWN_JOB");

        close(client_fd);
        return;
    }

    pthread_cond_init(&job->completed, NULL);

    printf("Received Job %d\n", id);

    enqueue_job(job);

    pthread_mutex_lock(&queue_mutex);

    while (job->status != JOB_COMPLETED &&
           job->status != JOB_FAILED) {

        pthread_cond_wait(&job->completed, &queue_mutex);
    }

    if (job->status == JOB_COMPLETED) {

        snprintf(response, sizeof(response),
                 "RESULT|%d|COMPLETED|%lld",
                 job->job_id, job->result);
    }
    else {

        snprintf(response, sizeof(response),
                 "RESULT|%d|FAILED",
                 job->job_id);
    }

    pthread_mutex_unlock(&queue_mutex);

    send_message(client_fd, response);

    printf("Result sent: %s\n", response);

    pthread_cond_destroy(&job->completed);

    free(job);

    close(client_fd);

    printf("Client disconnected\n");
}


/* Handle incoming connections */

void *handle_connection(void *arg)
{
    int socket_fd = *(int *)arg;

    free(arg);

    char buffer[MAX_MESSAGE_SIZE];

    int worker_id;

    printf("New connection received\n");

    if (receive_message(socket_fd, buffer,
                        sizeof(buffer)) < 0) {

        close(socket_fd);
        return NULL;
    }

    /* Worker registration */

    if (sscanf(buffer, "REGISTER|%d", &worker_id) == 1) {

        if (register_worker(worker_id, socket_fd) == 0) {

            char response[MAX_MESSAGE_SIZE];

            snprintf(response, sizeof(response),
                     "REGISTERED|%d", worker_id);

            send_message(socket_fd, response);

            printf("Worker %d connected and registered\n",
                   worker_id);

            notify_sdn_worker_status(worker_id, "available");

            return NULL;
        }

        send_message(socket_fd, "ERROR|REGISTRATION_FAILED");

        close(socket_fd);
        return NULL;
    }

    /* Client connection */

    if (strncmp(buffer, "SUBMIT|", 7) == 0) {

        printf("Client connected\n");

        handle_client(socket_fd, buffer);

        return NULL;
    }

    send_message(socket_fd, "ERROR|INVALID_REQUEST");

    close(socket_fd);

    return NULL;
}



/* UDP heartbeat listener */

void *udp_heartbeat_server(void *arg)
{
    (void)arg;

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("UDP socket creation failed");
        return NULL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8081);

    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("UDP bind failed");
        close(udp_fd);
        return NULL;
    }

    printf("UDP heartbeat listener active on port 8081\n");

    while (1) {
        char buffer[128];
        char response[128];
        struct sockaddr_in worker_addr;
        socklen_t addr_len = sizeof(worker_addr);

        ssize_t n = recvfrom(udp_fd, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&worker_addr, &addr_len);

        if (n <= 0)
            continue;

        buffer[n] = '\0';

        int worker_id;
        if (sscanf(buffer, "HEARTBEAT|%d", &worker_id) == 1) {
            update_worker_heartbeat(worker_id);
            snprintf(response, sizeof(response), "ACK|%d", worker_id);

            sendto(udp_fd, response, strlen(response), 0,
                   (struct sockaddr *)&worker_addr, addr_len);

            printf("UDP heartbeat received from Worker %d\n", worker_id);
        }
    }

    close(udp_fd);
    return NULL;
}


/* Periodically remove workers with missing heartbeats
   and notify the SDN controller after releasing registry mutex. */
void *heartbeat_monitor(void *arg)
{
    (void)arg;

    while (1) {
        sleep(2);

        unsigned int removed = remove_stale_workers(10);

        if (removed & (1U << 0))
            notify_sdn_worker_status(1, "unavailable");

        if (removed & (1U << 1))
            notify_sdn_worker_status(2, "unavailable");
    }

    return NULL;
}

/* Main server function */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int server_fd;

    struct sockaddr_in server_addr;

    pthread_t scheduler;
    pthread_t udp_thread;
    pthread_t monitor_thread;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {

        perror("Socket creation failed");

        return 1;
    }

    int reuse = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               &reuse, sizeof(reuse));

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {

        perror("Bind failed");

        close(server_fd);

        return 1;
    }

    if (listen(server_fd, 10) < 0) {

        perror("Listen failed");

        close(server_fd);

        return 1;
    }

    if (pthread_create(&scheduler, NULL,
                       scheduler_thread, NULL) != 0) {

        perror("Scheduler thread creation failed");

        close(server_fd);

        return 1;
    }


    pthread_detach(scheduler);

    if (pthread_create(&udp_thread, NULL,
                       udp_heartbeat_server, NULL) != 0) {
        perror("UDP heartbeat thread creation failed");
    } else {
        pthread_detach(udp_thread);
    }

    if (pthread_create(&monitor_thread, NULL,
                       heartbeat_monitor, NULL) != 0) {
        perror("Heartbeat monitor thread creation failed");
    } else {
        pthread_detach(monitor_thread);
    }

    printf("Distributed Job Manager listening on port %d\n",
           SERVER_PORT);

    while (1) {

        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {

            perror("Accept failed");

            continue;
        }

        int *connection_arg = malloc(sizeof(int));

        if (connection_arg == NULL) {

            close(client_fd);

            continue;
        }

        *connection_arg = client_fd;

        pthread_t connection_thread;

        if (pthread_create(&connection_thread, NULL,
                           handle_connection,
                           connection_arg) != 0) {

            perror("Connection thread creation failed");

            free(connection_arg);

            close(client_fd);

            continue;
        }

        pthread_detach(connection_thread);
    }

    close(server_fd);

    return 0;
}