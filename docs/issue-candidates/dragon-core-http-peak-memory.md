# Issue candidate: reduce dc_portal HTTP peak contiguous allocations

Repository: `dragon-core`

## Summary

`dc_portal` can return HTTP 500 `out of memory` during reconnect or concurrent
request bursts even though heap later recovers to a stable floor. The primary
target is `GET /api/v1/system/console`, which currently requests one contiguous
16,385-byte snapshot allocation. `GET /api/v1/provisioning` has a secondary peak
from holding its cJSON tree while allocating the serialized response.

This is a peak-allocation/fragmentation problem, not a demonstrated leak. Do not
address it merely by increasing a buffer or general heap reservation.

## Evidence

- Normal two-second console polling, repeated Setup loads, modal use and page
  refreshes reach a stable recovered heap floor.
- A mixed reconnect-style burst reproduced the exact 500 on 44/60 console
  requests and 3/60 provisioning requests; static SPA responses remained healthy.
- Heap recovered after request/socket cleanup.
- The generic `dc_ui` schema extension is not the root cause.

Full hardware measurements are recorded in DragonBreath's
`docs/HTTP_PEAK_MEMORY_FINDING.md`.

## Desired improvement

Reduce peak contiguous internal-heap demand in the shared response paths while
preserving authentication, response content, console snapshot consistency and
existing product behavior. Investigate the console endpoint first, then the
provisioning serialization peak.

Possible designs should be evaluated from measured heap behavior rather than
assumed to be correct. For example, bounded/chunked output may help, but snapshot
consistency and locking must be understood before selecting an implementation.

## Acceptance criteria

- No fixed 16,385-byte contiguous allocation is required to serve the console,
  or equivalent evidence shows the new design materially lowers its peak.
- Provisioning serialization peak is measured and reduced if it remains capable
  of failing after the console improvement.
- Repeated normal console polling and Setup use retain their current behavior.
- A concurrent reconnect/request stress test completes without HTTP OOM responses.
- Free heap and largest-free-block measurements reach a stable recovered floor.
- No product-specific policy is added to `dc_portal` or `dc_ui`.
- No controller, heater, safety or source-selection behavior changes.

