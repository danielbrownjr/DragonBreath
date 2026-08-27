# Transient HTTP peak-memory finding

Status: investigated and classified; no memory-management change is part of the
DragonBreath PID/UI feature work.

Hardware testing on 2026-08-27 used DragonBreath commit `e3d0f66` with `dc_ui`
pinned to dragon-core commit `f6368c3`. The device emitted:

```text
httpd_resp_send_err: 500 Internal Server Error - out of memory
```

The failure is a pre-existing transient HTTP peak-memory weakness, not evidence
of a persistent leak or a controller regression.

## Allocation paths

- `GET /api/v1/system/console` allocates a fixed, contiguous 16,385-byte
  (`DC_EVLOG_CONSOLE_BYTES + 1`) snapshot buffer before sending the console ring.
- `GET /api/v1/provisioning` constructs the Wi-Fi and product Setup descriptor as
  a cJSON tree, then allocates the serialized response while that tree is still
  resident. Under severe pressure, the response serialization can fail.
- The compressed `dc_ui` SPA is immutable and is sent directly from flash. It is
  not copied into an equivalent heap-sized response buffer.

## Reproduction and heap behavior

A clean reboot initially reported 80,584 bytes free and a 39,668-byte minimum.
With normal UI and SSE connections settled, free heap stabilized around 59-60 KB.

Normal usage did not show a downward trend:

- six PID confirmation/toggle cycles completed;
- six normal Setup opens and six full refreshes completed;
- 100 repeated provisioning descriptor requests completed without a failure;
- 40 console polls at the normal two-second cadence completed without a failure;
- free heap returned to approximately 59-60 KB after each workload.

A mixed reconnect-style burst reproduced the defect:

- `/api/v1/system/console`: 44 of 60 requests returned the exact HTTP 500;
- `/api/v1/provisioning`: 3 of 60 returned the exact HTTP 500 and 2 connections
  were dropped;
- static `/setup`: 30 of 30 requests succeeded;
- minimum free heap briefly reached 968 bytes.

After requests and sockets drained, free heap recovered to 60,060 bytes. Closing
the temporary test UI connection raised it to 70,084 bytes and reduced the SSE
client count. Request allocations, console snapshots, cJSON documents and SSE
connections therefore showed expected cleanup; there was no monotonic loss.

## dc_ui assessment

The generic `dc_ui` schema extension increased the compressed SPA by 1,240 bytes
(about 2.7%). The later backdrop-only cleanup added 21 compressed bytes. The live
provisioning response was approximately 4.8 KB; reconstructing the immediately
preceding product schema showed an estimated net increase of about 122 serialized
characters from the new schema fields.

Those changes add modest transfer and cJSON-node overhead, but the test evidence
does not identify them as the root cause. The dominant vulnerable allocation is
the console endpoint's contiguous 16,385-byte snapshot, followed by provisioning's
tree-plus-serialization peak when concurrent HTTP/socket pressure is already high.

Future work belongs in dragon-core and is captured separately in
[`issue-candidates/dragon-core-http-peak-memory.md`](issue-candidates/dragon-core-http-peak-memory.md).

