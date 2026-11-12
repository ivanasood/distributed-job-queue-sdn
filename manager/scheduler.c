
#include "../common/protocol.h"

#include <stdio.h>

long long execute_job(job_t *job)
{
    if (job == NULL) {
        return -1;
    }

    printf("Executing Job %d\n", job->job_id);

    if (job->type == JOB_SUM) {

        if (job->input1 > job->input2) {
            job->status = JOB_FAILED;
            return -1;
        }

        long long sum = 0;

        for (int i = job->input1; i <= job->input2; i++) {
            sum += i;
        }

        job->result = sum;
    }

    else if (job->type == JOB_FACTORIAL) {

        if (job->input1 < 0 || job->input1 > 20) {
            job->status = JOB_FAILED;
            return -1;
        }

        long long factorial = 1;

        for (int i = 1; i <= job->input1; i++) {
            factorial *= i;
        }

        job->result = factorial;
    }

    else if (job->type == JOB_PRIME) {

        int n = job->input1;
        int prime = 1;

        if (n < 2) {
            prime = 0;
        }

        for (int i = 2; prime && i <= n / i; i++) {
            if (n % i == 0) {
                prime = 0;
            }
        }

        job->result = prime;
    }

    else {
        job->status = JOB_FAILED;
        return -1;
    }

    job->status = JOB_COMPLETED;

    printf("Job %d completed successfully\n", job->job_id);

    return job->result;
}
