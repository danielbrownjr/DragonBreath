# RFC: Native Prusa (PrusaLink) control source

Status: **📋 Proposed — not started.** Tracked by
[issue #80](https://github.com/plastikman/DragonBreath/issues/80). No code yet; this
is the design record for when the work is picked up. A physical printer (Prusa Core
One) is needed for hardware validation — none on hand.

## Goal

Add **Prusa** as a first-class control source so a Panda Breath physically installed
in a Prusa Core One can drive its chamber heater **without any external hardware** —
DragonBreath polls PrusaLink directly over the LAN. This eliminates the current
Raspberry-Pi workaround and mirrors how Klipper / Bambu / HA already plug in.

## Background — what people do today

The [Panda Breath for Core One](https://www.printables.com/model/1742102) mod is
popular and well-documented, but software integration currently needs a Raspberry Pi
running [`Argolein/Panda-Breath-Prusa-Integration`](https://github.com/Argolein/Panda-Breath-Prusa-Integration).
That bridge is a **fake-Moonraker shim**: it polls PrusaLink for `temp_bed` /
`target_bed`, then stands up the tiny Moonraker JSON-RPC subset the Panda calls
(`printer.objects.query`) and answers with those values relabeled as
`heater_bed.temperature` / `heater_bed.target`. The Panda thinks it's talking to
Klipper. **It is bed-temperature only** — no filament, no other state.

Native support folds that Pi's whole job into the ESP32: `esp_http_client` polling
PrusaLink directly.

## PrusaLink API (verified against Prusa-Firmware-Buddy source — the Core One firmware)

Confirmed by reading the shipping firmware, not just the OpenAPI spec / third-party clients:

- **Plain HTTP, port 80, no TLS.** `esp_http_client` with zero cert handling. (`nhttp`
  is the on-device HTTP server.)
- **Auth: `X-Api-Key: <key>` on every `/api/*` request.** The key **is the printer's
  PrusaLink password**; missing/wrong → **HTTP 401**
  (`tests/integration/test_prusa_link.py`, `utils/gen-automata/http_server.py`). This is
  real and load-bearing in the shipping firmware, not just filabridge lore. The
  *documented* Digest scheme (`maker` / api-key, MD5 challenge) stays a **deferred
  fallback** — don't build it first.
- **`GET /api/v1/status`** — exact body from `lib/WUI/nhttp/status_renderer.cpp`:
  ```json
  { "job": { "id", "progress", "time_remaining", "time_printing" },
    "storage": {...}, "transfer": {...},
    "printer": {
      "state": "IDLE|PRINTING|PAUSED|FINISHED|STOPPED|READY|BUSY|ATTENTION",
      "temp_bed": 24.5, "target_bed": 0.0,        // floats @1dp; target_bed 0 = off
      "temp_nozzle": 0.0, "target_nozzle": 0.0, "axis_z": 0.0,
      "flow": 100, "speed": 100, "fan_hotend": 0, "fan_print": 0
  } }
  ```
  - Bed-follow needs only `printer.target_bed` (+ `temp_bed` for telemetry, `state` to
    optionally gate). State strings come from `printer_state::to_str`
    (`src/state/printer_state.cpp`): `IDLE / PRINTING / PAUSED / FINISHED / STOPPED /
    READY / BUSY / ATTENTION`.
  - **No chamber/enclosure temperature field — even on the enclosed Core One.** PrusaLink
    cannot report chamber temp, so DragonBreath's own chamber sensor stays the source of truth.
  - **No filament type** (see next section) — confirmed; `job` carries no dependable material.
- **Legacy OctoPrint API also present** (`/api/printer`, `/api/job` with `state.text`
  "Operational"/"Printing" + flags). We target the modern **`/api/v1/status`**: one call,
  cleaner shape, and it's what the Core One's own web UI uses.
- **Polling etiquette:** `nhttp` is socket-starved. Poll **no faster than ~2 s (5 s
  comfortable)** and **reuse one keep-alive TCP socket** across cycles rather than reconnecting.

## Developing without a printer (mock PrusaLink)

`tools/prusalink_mock.py` (this repo) stands in for a Core One exactly like Bambuddy did
for Bambu — it serves the verified `/api/v1/status` + `/api/version` shape, enforces
`X-Api-Key` → 401, and simulates a bed heat-soak (`temp_bed` ramps toward `target_bed`).
State/targets are drivable at runtime (`POST /mock/set`), so `dc_prusa` can be built and
exercised end-to-end on real ESP32 hardware against a laptop on the LAN. A Core One owner
only does the final real-printer confidence pass.

## The filament gap (the one real design constraint)

As of **1.1.7**, AUTO follows the loaded **filament's zone profile**, not the bed
threshold — bed-follow was removed as the global mechanism. This was intentional for
sources with rich data (Bambu, Klipper both report filament type). **PrusaLink does
not reliably expose filament type:**

- No "currently-loaded filament" concept at idle (no spool/AMS model).
- While printing, `job.file.meta["filament_type"]` *exists in the schema* but is
  **sparse/frequently absent on Buddy firmware** — filabridge distrusts it and
  parses the gcode header instead, which is too heavy/fragile for the C3.

So a Prusa source can give us a dependable **bed target**, but not a filament type.
That's the exact signal our new AUTO stopped consuming.

## Design — bed-follow as one source's currency, not a global mode

We do **not** re-introduce bed-threshold logic into `pb_policy`. The policy engine
already consumes a single value — `src_target_c` — and engages when it's `> 0`. It
doesn't care *how* that number was derived. Each source answers "what chamber temp
does this print want?" in whatever currency it can speak:

| Source | How it computes `src_target_c` |
|---|---|
| Bambu / Klipper (rich) | `dc_bambu_zone_target(material)` — filament zone |
| **Prusa** (bed only) | **bed-follow rule**, computed in the Prusa branch of `app_main` |

`pb_policy` is unchanged. The Prusa-specific behavior lives entirely in the Prusa
source path — it's just this source's way of producing `src_target_c`.

### Bed-follow needs two knobs (that zones supply for free)

Filament zones bundle "when to engage" and "hold what." Prusa has no filament to look
those up from, so they become **Prusa-source config fields**:

1. **Bed threshold** — engage when `printer.target_bed` ≥ X (default e.g. 60 °C).
2. **Chamber setpoint** — chamber temp to hold once engaged (default e.g. 45 °C).

Both user-overridable at `/setup`. This keeps the UX honest: "Prusa can't tell me the
filament, so tell me your bed→chamber rule once."

## What to build

**New `dc_prusa` core component** — modeled on `dc_moonraker`, but HTTP-poll instead
of WebSocket (the **first `esp_http_client` source** in the codebase):

- config `{ host, api_key, bed_threshold_c, chamber_target_c }`
- status `{ bed_temp, bed_target, state }`
- `dc_prusa_start()` spawns a ~5 s poll task (keep-alive socket); `dc_prusa_get_status()`
- ~250–350 lines; this is the bulk of the work.

**Integration points (all small, all existing patterns):**

- `dc_source`: add `DC_SRC_PRUSA = 5` (bump `DC_SRC_MAX` → 6) + a `dc_source_str` case.
- `app_main.c`: one `case DC_SRC_PRUSA` in the source switch — feed `bed_*` for
  telemetry and set `src_target_c` from the bed-follow rule (threshold → setpoint).
- `db_portal.c`: add "Prusa" to the `labels[]` selector, a
  `visible_when(ctl_src,"5")` section with `prusa_host` / `prusa_key` /
  `prusa_bed_threshold` / `prusa_chamber_target` fields, matching `PARSE_TEXT`/`PARSE_PORT`
  lines, and a `dc_prusa_set_config` in `apply_product`.
- UI: **automatic** — setup fields render from the descriptor; no `app.html` work.

## Effort

- **Bed-follow Prusa source (MANUAL + telemetry + AUTO via bed rule):** ~1 focused
  session once a printer is available for validation. The filament blocker dissolves —
  Prusa uses a bed rule instead of a zone, scoped to just this source.

## Non-goals

- **Filament-based AUTO over Prusa** — the API data isn't dependable enough; don't
  chase it. Bed-follow is the contract for this source.
- **Gcode-header parsing** on the ESP32 (filabridge's reliable filament path) — too
  heavy/fragile for the C3.
- **Sending control commands to the Prusa** — read-only, like Bambu.
- **Digest auth** — deferred; `X-Api-Key` covers current firmware.

## Open questions

- ~~Confirm `X-Api-Key` on the exact Core One firmware~~ — **RESOLVED** by reading
  Prusa-Firmware-Buddy: `X-Api-Key` gates every `/api/*` route, its value is the
  PrusaLink password, and a mismatch returns 401. Digest remains a deferred fallback
  only if a future firmware drops the header path.
- Default bed threshold / chamber setpoint values — pick sane defaults with a Core One
  owner rather than guessing.
- Whether to surface `printer.state` (PRINTING/PAUSED) to gate engagement, or engage
  purely on `target_bed ≥ threshold` (simpler; matches the RPi bridge's behavior).
