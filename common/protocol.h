
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* Server configuration */

#define SERVER_PORT 8080
#define MAX_MESSAGE_SIZE 1024
#define MAX_WORKERS 10
#define MAX_JOB_RETRIES 3

/* Job types */

typedef enum {
    JOB_SUM = 1,
    JOB_FACTORIAL,
    JOB_PRIME
} job_type_t;

/* Job status */

typedef enum {
    JOB_QUEUED = 1,
    JOB_RUNNING,
    JOB_COMPLETED,
    JOB_FAILED
} job_status_t;

/* Job structure */

typedef struct job {
    int job_id;
    job_type_t type;
    int input1;
    int input2;
    job_status_t status;
    long long result;
    int retry_count;
    pthread_cond_t completed;
    struct job *next;
} job_t;

/* Worker structure */

typedef struct {
    int worker_id;
    int socket_fd;
    int busy;
    int active_jobs;
    int completed_jobs;
    int failed_jobs;
    time_t last_heartbeat;
} worker_t;

/* Network statistics */

typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
} network_stats_t;

/* Message handling functions */

int send_message(int socket_fd, const char *message);
int receive_message(int socket_fd, char *buffer, int buffer_size);

/* Network monitoring functions */

void reset_socket_network_stats(int socket_fd);
void get_socket_network_stats(int socket_fd, network_stats_t *stats);

/* Job queue functions */

void enqueue_job(job_t *job);
job_t *dequeue_job(void);

/* Job execution function */

long long execute_job(job_t *job);

/* Worker registry functions */

int register_worker(int worker_id, int socket_fd);
int get_available_worker(int *worker_id, int *socket_fd);
void release_worker(int worker_id);
void remove_worker(int worker_id);
void update_worker_heartbeat(int worker_id);
unsigned int remove_stale_workers(int timeout_seconds);

void record_worker_completion(int worker_id);
void record_worker_failure(int worker_id);
void print_worker_status(void);

#endif
