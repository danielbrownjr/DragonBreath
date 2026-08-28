// SPDX-License-Identifier: MIT
#include "pb_sse_registry.h"

#include <string.h>

bool pb_sse_registry_reserve(pb_sse_registry_t *registry,
                             uint64_t connection_id, int socket_fd,
                             size_t *slot, unsigned *clients_before,
                             unsigned *clients_after)
{
    if (!registry || !slot || !clients_before || !clients_after ||
            connection_id == 0) return false;

    *clients_before = registry->count;
    *clients_after = registry->count;
    *slot = PB_SSE_MAX_CLIENTS;
    if (registry->count >= PB_SSE_MAX_CLIENTS) return false;

    for (size_t i = 0; i < PB_SSE_MAX_CLIENTS; i++) {
        if (!registry->clients[i].active) {
            *slot = i;
            break;
        }
    }
    if (*slot == PB_SSE_MAX_CLIENTS) return false;

    registry->clients[*slot] = (pb_sse_client_t) {
        .active = true,
        .connection_id = connection_id,
        .socket_fd = socket_fd,
        .request = NULL,
    };
    registry->count++;
    *clients_after = registry->count;
    return true;
}

bool pb_sse_registry_attach(pb_sse_registry_t *registry, size_t slot,
                            uint64_t connection_id, void *request)
{
    if (!registry || !request || slot >= PB_SSE_MAX_CLIENTS) return false;
    pb_sse_client_t *client = &registry->clients[slot];
    if (!client->active || client->connection_id != connection_id) return false;
    client->request = request;
    return true;
}

bool pb_sse_registry_release(pb_sse_registry_t *registry, size_t slot,
                             uint64_t connection_id,
                             unsigned *clients_before,
                             unsigned *clients_after)
{
    if (!registry || !clients_before || !clients_after) return false;
    *clients_before = registry->count;

    bool matched = slot < PB_SSE_MAX_CLIENTS &&
        registry->clients[slot].active &&
        registry->clients[slot].connection_id == connection_id;
    if (matched) {
        memset(&registry->clients[slot], 0, sizeof registry->clients[slot]);
        if (registry->count > 0) registry->count--;
    }
    *clients_after = registry->count;
    return matched;
}
