# SSE transport investigation

This investigation is isolated from chamber PID/controller work. The server-side
transport belongs to DragonBreath's `pb_httpd`; DragonSniff is the read-only churn
client and evidence recorder. Dragon-core only supplies an optional browser
`EventSource` consumer and is not modified here.

## Instrumented baseline

- DragonBreath base: upstream `main` at `a9a9999b7d1036be3ae274475925ea17478d66e0`
- Server endpoint: `GET /api/v2/events`
- Capacity policy: two registered SSE clients; further attempts receive HTTP 503
- Client runner: DragonSniff `feature/churn-test-profiles` at `0078630`

The firmware logs one lifecycle sequence per monotonically increasing connection
ID. `/api/v2/health` exposes current slots, cumulative accept/reject/failure/cleanup
counters, heap headroom, HTTP-task stack headroom, and total task count. This is
temporary characterization instrumentation, not a new product API contract.

## Hardware reproduction procedure

Keep the heater cold and retain both the DragonBreath serial console and
DragonSniff JSONL evidence for every run.

1. Reboot once, wait for Wi-Fi and the portal to settle, then close all product UI
   pages. Record `boot_id` and ten health samples over at least 20 seconds. Confirm
   `sse_clients=0` and an empty `sse_connections` array.
2. Open one long-lived SSE observer. Confirm one health slot with a stable
   connection ID/fd and increasing `successful_events`. Repeat with a second
   observer. A third connection must receive the documented HTTP 503 while the two
   existing streams remain healthy.
3. Close each observer gracefully. Record the time of client close, device
   `peer disconnected`, `cleanup complete`, the next health sample, and the first
   successful replacement connection. This gives disconnect-to-capacity-reuse
   latency.
4. With no other SSE owner, run DragonSniff Baseline, Extended, and Stress profiles.
   Repeat custom runs at 0.1, 0.5, 1.0, and 2.0 second inter-cycle delays. The
   current bounded runner intentionally does not offer a zero-delay setting; do not
   silently weaken that guard for this investigation.
5. Repeat a bounded run while one unrelated SSE observer remains connected. The
   DragonSniff stream should occupy/release the second slot without disturbing the
   first. Then repeat with two unrelated observers to verify that capacity
   rejection is clean and does not corrupt either established response.
6. During stable streams and churn, repeatedly request `/api/v2/health`,
   `/api/v2/state`, and `/api/v2/info`. Record HTTP status, latency, and any
   `BadStatusLine` exception exactly as DragonSniff stores it.
7. Repeat one graceful-close run using abrupt client termination, then one brief
   Wi-Fi interruption if practical. Do not power-cycle merely to clear a slot.
8. After churn stops, sample health until `sse_clients=0` and
   `sse_connections=[]`. Continue for at least 60 seconds to establish the recovered
   free-heap/largest-block/task-count floor.

## Evidence interpretation

A 503 is legitimate capacity behavior only when the device log shows a rejected
connection while two distinct active slot owners remain healthy. A 503 with fewer
than two active slot records, or an accepted count that cannot be reconciled with
the slots, indicates bookkeeping/cleanup failure.

Treat `BadStatusLine` separately unless its timestamp correlates with a specific
server lifecycle anomaly. Correlating evidence includes reused fds with overlapping
connection generations, a send after the former slot was cleaned, a send-stage
failure on the same generation, `slot_matched=false`, or an async-completion error.
Without such correlation, capacity rejection and response corruption remain two
distinct observations.

## Acceptance criteria for a future fix

- Exactly two healthy long-lived clients remain independent.
- Every additional connection gets a complete HTTP 503 response, never malformed
  status bytes or partial SSE data.
- Graceful and abrupt disconnects reclaim only their owning generation/slot.
- A reclaimed slot becomes reusable within the measured cleanup bound, with no
  stale slot and no duplicate active owner.
- Normal HTTP responses remain valid while one or two SSE streams are active and
  during bounded churn.
- No `BadStatusLine`, mixed response, panic, watchdog, or reboot occurs.
- Accepted minus cleanups equals the current registered-client count after allowing
  for a connection still in startup/cleanup.
- Free heap, largest free block, task count, and HTTP-task stack headroom recover to
  a stable floor rather than degrading monotonically across repeated runs.

## Current characterization boundary

No transport fix is proposed until a hardware run correlates client evidence with
the generation/fd/slot lifecycle logs. The smallest likely server-side area is
`events_get()`, `sse_task()`, `sse_send()`, and async request completion in
`components/pb_httpd/pb_httpd.c`. DragonSniff's existing client already records raw
503 bodies, status/headers, connection timing, exact exception type/text, cleanup,
health snapshots, and local permit/thread release.
