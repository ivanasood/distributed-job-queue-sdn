
#include "protocol.h"

#include <string.h>
#include <sys/socket.h>
#include <pthread.h>

#define MAX_TRACKED_SOCKETS 256

typedef struct {
    int socket_fd;
    int active;
    network_stats_t stats;
} socket_entry_t;

static socket_entry_t socket_stats[MAX_TRACKED_SOCKETS];

static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Find or create a statistics entry */

static socket_entry_t *find_socket_locked(int socket_fd)
{
    socket_entry_t *free_entry = NULL;

    for (int i = 0; i < MAX_TRACKED_SOCKETS; i++) {

        if (socket_stats[i].active &&
            socket_stats[i].socket_fd == socket_fd) {
            return &socket_stats[i];
        }

        if (!socket_stats[i].active && free_entry == NULL) {
            free_entry = &socket_stats[i];
        }
    }

    if (free_entry != NULL) {
        free_entry->socket_fd = socket_fd;
        free_entry->active = 1;

        memset(&free_entry->stats, 0,
               sizeof(free_entry->stats));

        return free_entry;
    }

    return NULL;
}

/* Reset statistics when a socket is registered */

void reset_socket_network_stats(int socket_fd)
{
    pthread_mutex_lock(&stats_mutex);

    socket_entry_t *entry = find_socket_locked(socket_fd);

    if (entry != NULL) {
        memset(&entry->stats, 0, sizeof(entry->stats));
    }

    pthread_mutex_unlock(&stats_mutex);
}

/* Retrieve statistics for a socket */

void get_socket_network_stats(int socket_fd,
                              network_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    pthread_mutex_lock(&stats_mutex);

    for (int i = 0; i < MAX_TRACKED_SOCKETS; i++) {

        if (socket_stats[i].active &&
            socket_stats[i].socket_fd == socket_fd) {

            *stats = socket_stats[i].stats;
            break;
        }
    }

    pthread_mutex_unlock(&stats_mutex);
}

/* Record transmitted bytes */

static void record_sent(int socket_fd, size_t bytes)
{
    pthread_mutex_lock(&stats_mutex);

    socket_entry_t *entry = find_socket_locked(socket_fd);

    if (entry != NULL) {
        entry->stats.bytes_sent += bytes;
    }

    pthread_mutex_unlock(&stats_mutex);
}

/* Record received bytes */

static void record_received(int socket_fd, size_t bytes)
{
    pthread_mutex_lock(&stats_mutex);

    socket_entry_t *entry = find_socket_locked(socket_fd);

    if (entry != NULL) {
        entry->stats.bytes_received += bytes;
    }

    pthread_mutex_unlock(&stats_mutex);
}

/* Record completed messages */

static void record_message_sent(int socket_fd)
{
    pthread_mutex_lock(&stats_mutex);

    socket_entry_t *entry = find_socket_locked(socket_fd);

    if (entry != NULL) {
        entry->stats.messages_sent++;
    }

    pthread_mutex_unlock(&stats_mutex);
}

static void record_message_received(int socket_fd)
{
    pthread_mutex_lock(&stats_mutex);

    socket_entry_t *entry = find_socket_locked(socket_fd);

    if (entry != NULL) {
        entry->stats.messages_received++;
    }

    pthread_mutex_unlock(&stats_mutex);
}

/* Send a newline-delimited TCP message */

int send_message(int socket_fd, const char *message)
{
    size_t length = strlen(message);
    size_t sent = 0;

    if (length == 0 || length >= MAX_MESSAGE_SIZE) {
        return -1;
    }

    char framed[MAX_MESSAGE_SIZE];

    memcpy(framed, message, length);
    framed[length] = '\n';
    length++;

    while (sent < length) {

        ssize_t result = send(socket_fd, framed + sent,
                              length - sent, 0);

        if (result <= 0) {
            return -1;
        }

        sent += (size_t)result;

        record_sent(socket_fd, (size_t)result);
    }

    record_message_sent(socket_fd);

    return 0;
}

/* Receive a newline-delimited TCP message */

int receive_message(int socket_fd, char *buffer, int buffer_size)
{
    int index = 0;
    char ch;

    if (buffer_size < 2) {
        return -1;
    }

    while (index < buffer_size - 1) {

        ssize_t result = recv(socket_fd, &ch, 1, 0);

        if (result <= 0) {
            return -1;
        }

        record_received(socket_fd, (size_t)result);

        if (ch == '\n') {

            buffer[index] = '\0';

            record_message_received(socket_fd);

            return index;
        }

        buffer[index++] = ch;
    }

    buffer[index] = '\0';

    return -1;
}
