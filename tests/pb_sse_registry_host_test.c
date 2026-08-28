#include "pb_sse_registry.h"

#include <assert.h>
#include <stdio.h>

static size_t reserve(pb_sse_registry_t *registry, uint64_t id, int fd,
                      unsigned before, unsigned after)
{
    size_t slot;
    unsigned actual_before;
    unsigned actual_after;
    assert(pb_sse_registry_reserve(registry, id, fd, &slot,
                                   &actual_before, &actual_after));
    assert(actual_before == before);
    assert(actual_after == after);
    assert(registry->count == after);
    assert(slot < PB_SSE_MAX_CLIENTS);
    assert(registry->clients[slot].active);
    assert(registry->clients[slot].connection_id == id);
    assert(registry->clients[slot].socket_fd == fd);
    return slot;
}

int main(void)
{
    pb_sse_registry_t registry = {0};

    size_t first = reserve(&registry, 1, 53, 0, 1);
    size_t second = reserve(&registry, 2, 55, 1, 2);
    assert(first != second);
    assert(pb_sse_registry_attach(&registry, first, 1, (void *)1));
    assert(pb_sse_registry_attach(&registry, second, 2, (void *)2));

    // A client at the configured limit is rejected without disturbing either
    // accepted connection or letting the count exceed the maximum.
    size_t rejected_slot = 0;
    unsigned before = 0;
    unsigned after = 0;
    assert(!pb_sse_registry_reserve(&registry, 3, 54, &rejected_slot,
                                    &before, &after));
    assert(rejected_slot == PB_SSE_MAX_CLIENTS);
    assert(before == PB_SSE_MAX_CLIENTS && after == PB_SSE_MAX_CLIENTS);
    assert(registry.clients[first].connection_id == 1);
    assert(registry.clients[second].connection_id == 2);

    // Cleanup for a stale generation cannot release or decrement the slot now
    // owned by another connection.
    assert(!pb_sse_registry_release(&registry, first, 99, &before, &after));
    assert(before == 2 && after == 2);
    assert(registry.clients[first].active);

    assert(pb_sse_registry_release(&registry, first, 1, &before, &after));
    assert(before == 2 && after == 1);
    size_t reused = reserve(&registry, 4, 53, 1, 2);
    assert(reused == first);

    // A late cleanup from the old generation cannot remove the reused slot,
    // even when the operating system has reused the same descriptor number.
    assert(!pb_sse_registry_release(&registry, first, 1, &before, &after));
    assert(before == 2 && after == 2);
    assert(registry.clients[first].connection_id == 4);

    assert(pb_sse_registry_release(&registry, second, 2, &before, &after));
    assert(pb_sse_registry_release(&registry, reused, 4, &before, &after));
    assert(registry.count == 0);

    // Rapid reserve/release cycles retain a bounded, exact count.
    for (uint64_t id = 5; id < 1005; id++) {
        size_t slot = reserve(&registry, id, (int)(50 + id % 4), 0, 1);
        assert(pb_sse_registry_release(&registry, slot, id, &before, &after));
        assert(before == 1 && after == 0);
    }

    puts("SSE registry lifecycle tests: PASS");
    return 0;
}
