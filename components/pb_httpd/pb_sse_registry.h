// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Two task-backed streams are the established resource budget. Additional UI
// clients fall back to the read-only state endpoint rather than consuming a task.
#define PB_SSE_MAX_CLIENTS 2

typedef struct {
    bool active;
    uint64_t connection_id;
    int socket_fd;
    void *request;
} pb_sse_client_t;

typedef struct {
    unsigned count;
    pb_sse_client_t clients[PB_SSE_MAX_CLIENTS];
} pb_sse_registry_t;

// These helpers do not lock. The owner must serialize every operation and any
// direct access to the registry. A generation ID prevents stale cleanup from
// releasing or decrementing a slot now owned by another connection.
bool pb_sse_registry_reserve(pb_sse_registry_t *registry,
                             uint64_t connection_id, int socket_fd,
                             size_t *slot, unsigned *clients_before,
                             unsigned *clients_after);
bool pb_sse_registry_attach(pb_sse_registry_t *registry, size_t slot,
                            uint64_t connection_id, void *request);
bool pb_sse_registry_release(pb_sse_registry_t *registry, size_t slot,
                             uint64_t connection_id,
                             unsigned *clients_before,
                             unsigned *clients_after);
