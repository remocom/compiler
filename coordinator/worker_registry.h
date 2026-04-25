#ifndef REMOCOM_WORKER_REGISTRY_H
#define REMOCOM_WORKER_REGISTRY_H

#include <netinet/in.h>

#include "coordinator_types.h"

typedef void (*WorkerRegistryLogFn)(void *callback_ctx, const char *message);
typedef void (*WorkerRegistryRequeueFn)(void *callback_ctx, const CompileTask *task);

/// @brief Callbacks used by the worker registry for logging and task requeueing.
typedef struct {
    WorkerRegistryLogFn log_message;
    WorkerRegistryRequeueFn requeue_task;
    void *callback_ctx;
} WorkerRegistryConfig;

/// @brief Represents a worker node's status.
typedef enum {
    WORKER_STATUS_DEAD,
    WORKER_STATUS_ALIVE
} WorkerStatus;

/// @brief Stable worker state copied out of the registry for message dispatch.
typedef struct {
    int node_id;
    WorkerStatus status;
    int handshake_completed;
} WorkerSnapshot;

/// @brief Configures callbacks used by the worker registry.
/// @param config The configuration containing the callbacks to set.
void remocom_worker_registry_configure(const WorkerRegistryConfig *config);

/// @brief Looks up a worker by socket and copies the dispatch-relevant state.
/// @param client_fd The socket file descriptor of the worker to look up.
/// @param snapshot Output parameter for the worker snapshot.
/// @return 1 if the worker was found, 0 otherwise.
int remocom_worker_registry_get_snapshot(int client_fd, WorkerSnapshot *snapshot);

/// @brief Marks a worker handshake as completed.
/// @param client_fd The socket file descriptor of the worker to update.
void remocom_worker_registry_mark_handshake_completed(int client_fd);

/// @brief Refreshes a worker heartbeat timestamp.
/// @param client_fd The socket file descriptor of the worker to update.
void remocom_worker_registry_update_heartbeat(int client_fd);

/// @brief Records the task currently assigned to a worker.
/// @param callback_ctx The callback context for the registry configuration.
/// @param client_fd The socket file descriptor of the worker to update.
/// @param task The task to mark as active for the worker.
void remocom_worker_registry_mark_task_active(void *callback_ctx, int client_fd, const CompileTask *task);

/// @brief Clears a worker's active-task marker.
/// @param client_fd The socket file descriptor of the worker to update.
void remocom_worker_registry_clear_active_task(int client_fd);

/// @brief Registers a newly connected worker.
/// @param client_fd The socket file descriptor of the worker to register.
/// @param client_addr The socket address of the worker to register.
/// @return 1 if registered, 0 if rejected.
int remocom_worker_registry_register(int client_fd, const struct sockaddr_in *client_addr);

/// @brief Removes a worker by socket and requeues any active task.
/// @param client_fd The socket file descriptor of the worker to remove.
void remocom_worker_registry_remove_by_socket(int client_fd);

/// @brief Removes a worker record after thread creation failed.
/// @param client_fd The socket file descriptor of the worker to remove.
void remocom_worker_registry_remove_for_thread_failure(int client_fd);

/// @brief Monitors worker heartbeats and marks timed-out workers dead.
void *remocom_worker_registry_monitor(void *arg);

#endif
