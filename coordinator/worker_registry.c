#include "worker_registry.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 100
#define WORKER_REGISTRY_LOG_SIZE 1024

/// @brief Represents a worker node in the system.
typedef struct {
    int nodeID;
    int socketID;
    char ip_address[INET_ADDRSTRLEN];
    time_t last_heartbeat;
    WorkerStatus status;
    int handshake_completed;
    int has_active_task;
    CompileTask current_task;
} WorkerNode;

/// @brief Represents a request to requeue a task when a worker is removed.
typedef struct {
    int has_task;
    CompileTask task;
} RequeueRequest;

static WorkerNode workers[MAX_WORKERS];
static int worker_count = 0;
static pthread_mutex_t workers_mutex = PTHREAD_MUTEX_INITIALIZER;
static WorkerRegistryConfig registry_config;

/// @brief Logs a message using the configured logging callback.
/// @param message The message to log.
static void registry_log(const char *message) {
    if (registry_config.log_message != NULL) {
        registry_config.log_message(registry_config.callback_ctx, message);
    }
}

/// @brief Formats and logs a message using the configured logging callback.
/// @param format The printf-style format string.
/// @param ... The arguments for the format string.
static void registry_logf(const char *format, ...) {
    char message[WORKER_REGISTRY_LOG_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    registry_log(message);
}

/// @brief Finds a worker node by its socket file descriptor.
/// @param client_fd The socket file descriptor of the worker to find.
/// @return A pointer to the worker node if found, NULL otherwise.
static WorkerNode *find_worker_by_socket_unlocked(int client_fd) {
    for (int i = 0; i < worker_count; i++) {
        if (workers[i].socketID == client_fd) {
            return &workers[i];
        }
    }
    return NULL;
}

/// @brief Requeues a task if a worker is removed.
/// @param request The requeue request containing the task to requeue.
static void requeue_task_if_needed(const RequeueRequest *request) {
    if (request->has_task && registry_config.requeue_task != NULL) {
        registry_config.requeue_task(registry_config.callback_ctx, &request->task);
    }
}

void remocom_worker_registry_configure(const WorkerRegistryConfig *config) {
    if (config == NULL) {
        memset(&registry_config, 0, sizeof(registry_config));
        return;
    }

    registry_config = *config;
}

int remocom_worker_registry_get_snapshot(int client_fd, WorkerSnapshot *snapshot) {
    int found = 0;

    pthread_mutex_lock(&workers_mutex);
    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL && snapshot != NULL) {
        snapshot->node_id = worker->nodeID;
        snapshot->status = worker->status;
        snapshot->handshake_completed = worker->handshake_completed;
        found = 1;
    }
    pthread_mutex_unlock(&workers_mutex);

    return found;
}

void remocom_worker_registry_mark_handshake_completed(int client_fd) {
    pthread_mutex_lock(&workers_mutex);
    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL) {
        worker->handshake_completed = 1;
    }
    pthread_mutex_unlock(&workers_mutex);
}

void remocom_worker_registry_update_heartbeat(int client_fd) {
    pthread_mutex_lock(&workers_mutex);
    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL) {
        worker->last_heartbeat = time(NULL);
    }
    pthread_mutex_unlock(&workers_mutex);
}

void remocom_worker_registry_mark_task_active(void *callback_ctx, int client_fd, const CompileTask *task) {
    (void)callback_ctx;

    pthread_mutex_lock(&workers_mutex);
    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL && task != NULL) {
        worker->current_task = *task;
        worker->has_active_task = 1;
    }
    pthread_mutex_unlock(&workers_mutex);
}

void remocom_worker_registry_clear_active_task(int client_fd) {
    pthread_mutex_lock(&workers_mutex);
    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL) {
        worker->has_active_task = 0;
    }
    pthread_mutex_unlock(&workers_mutex);
}

int remocom_worker_registry_register(int client_fd, const struct sockaddr_in *client_addr) {
    pthread_mutex_lock(&workers_mutex);

    // Reject new connections if we've reached the maximum worker limit.
    if (worker_count >= MAX_WORKERS) {
        pthread_mutex_unlock(&workers_mutex);
        registry_log("[WARNING] Rejecting connection: max worker limit reached\n");
        close(client_fd);
        return 0;
    }

    WorkerNode *worker = &workers[worker_count];
    worker->socketID = client_fd;
    worker->nodeID = worker_count;
    worker->last_heartbeat = time(NULL);
    worker->status = WORKER_STATUS_ALIVE;
    worker->handshake_completed = 0;
    worker->has_active_task = 0;

    // Convert the client's IP address to a string and store it in the worker node.
    inet_ntop(AF_INET, &client_addr->sin_addr, worker->ip_address, INET_ADDRSTRLEN);

    printf("Worker added. Total workers: %d\n", worker_count + 1);
    registry_logf("CONNECT Node %d | IP: %s | Socket: %d\n",
        worker->nodeID, worker->ip_address, worker->socketID);

    worker_count++;
    pthread_mutex_unlock(&workers_mutex);
    return 1;
}

void remocom_worker_registry_remove_by_socket(int client_fd) {
    RequeueRequest requeue = {0};

    pthread_mutex_lock(&workers_mutex);

    // Find the worker by socket and remove it from the registry.
    for (int i = 0; i < worker_count; i++) {
        WorkerNode *worker = &workers[i];
        if (worker->socketID != client_fd) {
            continue;
        }

        if (worker->status == WORKER_STATUS_DEAD) {
            registry_logf("REMOVED (timeout) Node %d | IP: %s\n", worker->nodeID, worker->ip_address);
        } else {
            registry_logf("DISCONNECT Node %d | IP: %s | Socket: %d\n",
                worker->nodeID, worker->ip_address, worker->socketID);

            // If the worker had an active task, prepare to requeue it after removing the worker.
            if (worker->has_active_task) {
                registry_logf("RE-ENQUEUE task for reassignment | Node %d | Source: %s\n",
                    worker->nodeID, worker->current_task.source_path);
                requeue.has_task = 1;
                requeue.task = worker->current_task;
                worker->has_active_task = 0;
            }
        }

        *worker = workers[worker_count - 1];
        worker_count--;
        break;
    }

    printf("Worker removed. Total workers: %d\n", worker_count);
    pthread_mutex_unlock(&workers_mutex);

    requeue_task_if_needed(&requeue);
}

void remocom_worker_registry_remove_for_thread_failure(int client_fd) {
    pthread_mutex_lock(&workers_mutex);

    WorkerNode *worker = find_worker_by_socket_unlocked(client_fd);
    if (worker != NULL) {
        registry_logf("[ERROR] Failed to connect Node %d to thread.\n", worker->nodeID);
        *worker = workers[worker_count - 1];
        worker_count--;
    }

    pthread_mutex_unlock(&workers_mutex);
}

void *remocom_worker_registry_monitor(void *arg) {
    (void)arg;

    while (1) {
        sleep(10);
        time_t current_time = time(NULL);
        RequeueRequest requeues[MAX_WORKERS];
        int requeue_count = 0;

        pthread_mutex_lock(&workers_mutex);

        // Check for workers that have not sent a heartbeat recently and mark them as dead.
        for (int i = 0; i < worker_count; i++) {
            WorkerNode *worker = &workers[i];
            int time_since_heartbeat = (int)(current_time - worker->last_heartbeat);

            if (time_since_heartbeat > 15 && worker->status != WORKER_STATUS_DEAD) {
                registry_logf("[TIMEOUT] Node %d | IP: %s\n", worker->nodeID, worker->ip_address);
                worker->status = WORKER_STATUS_DEAD;
                close(worker->socketID);

                // If the worker had an active task, prepare to requeue it after
                // marking the worker as dead.
                if (worker->has_active_task && requeue_count < MAX_WORKERS) {
                    registry_logf("RE-ENQUEUE task for reassignment | Node %d | Source: %s\n",
                        worker->nodeID, worker->current_task.source_path);
                    requeues[requeue_count].has_task = 1;
                    requeues[requeue_count].task = worker->current_task;
                    requeue_count++;
                    worker->has_active_task = 0;
                }
            }
        }
        pthread_mutex_unlock(&workers_mutex);

        for (int i = 0; i < requeue_count; i++) {
            requeue_task_if_needed(&requeues[i]);
        }
    }

    return NULL;
}
