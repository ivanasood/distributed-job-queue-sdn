
#include "../common/protocol.h"

#include <stdio.h>
#include <unistd.h>

static worker_t workers[MAX_WORKERS];

static int worker_count = 0;
static int next_worker_index = 0;

static pthread_mutex_t worker_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Display worker workload and network statistics */

static void print_worker_status_locked(void)
{
    printf("\n========== WORKER LOAD STATUS ==========\n");

    if (worker_count == 0) {
        printf("No workers registered.\n");
    }

    for (int i = 0; i < worker_count; i++) {

        network_stats_t stats;

        get_socket_network_stats(
            workers[i].socket_fd, &stats);

        printf("\nWorker %d\n", workers[i].worker_id);

        printf("Status: %s\n",
               workers[i].busy ? "BUSY" : "AVAILABLE");

        printf("Active Jobs: %d\n",
               workers[i].active_jobs);

        printf("Completed Jobs: %d\n",
               workers[i].completed_jobs);

        printf("Failed Jobs: %d\n",
               workers[i].failed_jobs);

        printf("Network Traffic:\n");

        printf("  Bytes Sent: %llu\n",
               (unsigned long long)stats.bytes_sent);

        printf("  Bytes Received: %llu\n",
               (unsigned long long)stats.bytes_received);

        printf("  Messages Sent: %llu\n",
               (unsigned long long)stats.messages_sent);

        printf("  Messages Received: %llu\n",
               (unsigned long long)stats.messages_received);

        printf("  Total Bytes: %llu\n",
               (unsigned long long)(
                   stats.bytes_sent + stats.bytes_received));
    }

    printf("\n========================================\n\n");
}

/* Register a new worker */

int register_worker(int worker_id, int socket_fd)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {

        if (workers[i].worker_id == worker_id) {
            pthread_mutex_unlock(&worker_mutex);
            return -1;
        }
    }

    if (worker_count >= MAX_WORKERS) {
        pthread_mutex_unlock(&worker_mutex);
        return -1;
    }

    workers[worker_count].worker_id = worker_id;
    workers[worker_count].socket_fd = socket_fd;
    workers[worker_count].busy = 0;
    workers[worker_count].active_jobs = 0;
    workers[worker_count].completed_jobs = 0;
    workers[worker_count].failed_jobs = 0;
    workers[worker_count].last_heartbeat = time(NULL);

    reset_socket_network_stats(socket_fd);

    printf("Worker %d registered successfully\n", worker_id);

    worker_count++;

    print_worker_status_locked();

    pthread_mutex_unlock(&worker_mutex);

    return 0;
}

/* Get an available worker using Round Robin */

int get_available_worker(int *worker_id, int *socket_fd)
{
    pthread_mutex_lock(&worker_mutex);

    if (worker_count == 0) {
        pthread_mutex_unlock(&worker_mutex);
        return -1;
    }

    for (int i = 0; i < worker_count; i++) {

        int index = (next_worker_index + i) % worker_count;

        if (workers[index].busy == 0) {

            workers[index].busy = 1;
            workers[index].active_jobs = 1;

            *worker_id = workers[index].worker_id;
            *socket_fd = workers[index].socket_fd;

            next_worker_index = (index + 1) % worker_count;

            print_worker_status_locked();

            pthread_mutex_unlock(&worker_mutex);

            return 0;
        }
    }

    pthread_mutex_unlock(&worker_mutex);

    return -1;
}

/* Record a successfully completed job */

void record_worker_completion(int worker_id)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {

        if (workers[i].worker_id == worker_id) {
            workers[i].completed_jobs++;
            break;
        }
    }

    pthread_mutex_unlock(&worker_mutex);
}

/* Record a failed job */

void record_worker_failure(int worker_id)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {

        if (workers[i].worker_id == worker_id) {
            workers[i].failed_jobs++;
            break;
        }
    }

    pthread_mutex_unlock(&worker_mutex);
}

/* Mark a worker as available */

void release_worker(int worker_id)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {

        if (workers[i].worker_id == worker_id) {

            workers[i].busy = 0;
            workers[i].active_jobs = 0;

            break;
        }
    }

    print_worker_status_locked();

    pthread_mutex_unlock(&worker_mutex);
}

/* Public function to display worker status */

void print_worker_status(void)
{
    pthread_mutex_lock(&worker_mutex);

    print_worker_status_locked();

    pthread_mutex_unlock(&worker_mutex);
}

/* Remove a disconnected or failed worker */

void remove_worker(int worker_id)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {

        if (workers[i].worker_id == worker_id) {

            printf("Removing Worker %d from registry\n",
                   worker_id);

            close(workers[i].socket_fd);

            if (i < next_worker_index) {
                next_worker_index--;
            }

            workers[i] = workers[worker_count - 1];

            worker_count--;

            if (worker_count == 0) {
                next_worker_index = 0;
            }
            else if (next_worker_index >= worker_count) {
                next_worker_index = 0;
            }

            printf("Worker %d removed successfully\n",
                   worker_id);

            break;
        }
    }

    print_worker_status_locked();

    pthread_mutex_unlock(&worker_mutex);
}


/* Update the last heartbeat timestamp for a registered worker */
void update_worker_heartbeat(int worker_id)
{
    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; i++) {
        if (workers[i].worker_id == worker_id) {
            workers[i].last_heartbeat = time(NULL);
            break;
        }
    }

    pthread_mutex_unlock(&worker_mutex);
}

/* Remove idle workers whose heartbeat has timed out.
   Return a bitmask: bit 0 = Worker 1, bit 1 = Worker 2. */
unsigned int remove_stale_workers(int timeout_seconds)
{
    time_t now = time(NULL);
    unsigned int removed_mask = 0;

    pthread_mutex_lock(&worker_mutex);

    for (int i = 0; i < worker_count; ) {
        if (!workers[i].busy &&
            now - workers[i].last_heartbeat > timeout_seconds) {

            int stale_id = workers[i].worker_id;

            printf("Heartbeat timeout: removing Worker %d\n", stale_id);

            if (stale_id >= 1 && stale_id <= 2)
                removed_mask |= (1U << (stale_id - 1));

            close(workers[i].socket_fd);

            if (i < next_worker_index)
                next_worker_index--;

            workers[i] = workers[worker_count - 1];
            worker_count--;

            if (worker_count == 0)
                next_worker_index = 0;
            else if (next_worker_index >= worker_count)
                next_worker_index = 0;

            printf("Worker %d removed after heartbeat timeout\n",
                   stale_id);
        } else {
            i++;
        }
    }

    if (removed_mask != 0)
        print_worker_status_locked();

    pthread_mutex_unlock(&worker_mutex);

    return removed_mask;
}
