
#include "../common/protocol.h"

#include <stdio.h>

static job_t *front = NULL;
static job_t *rear = NULL;

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;

void enqueue_job(job_t *job)
{
    if (job == NULL) {
        return;
    }

    pthread_mutex_lock(&queue_mutex);

    job->next = NULL;
    job->status = JOB_QUEUED;

    if (front == NULL) {
        front = rear = job;
    }
    else {
        rear->next = job;
        rear = job;
    }

    printf("Job %d added to queue\n", job->job_id);

    pthread_cond_signal(&queue_not_empty);

    pthread_mutex_unlock(&queue_mutex);
}

job_t *dequeue_job(void)
{
    pthread_mutex_lock(&queue_mutex);

    while (front == NULL) {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    job_t *job = front;

    front = front->next;

    if (front == NULL) {
        rear = NULL;
    }

    job->next = NULL;
    job->status = JOB_RUNNING;

    printf("Job %d removed from queue\n", job->job_id);

    pthread_mutex_unlock(&queue_mutex);

    return job;
}
