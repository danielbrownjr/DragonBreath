# RFC: Decouple `pb_httpd` from the heater via a device interface

Status: **Draft / design.** Break the HTTP/OTA layer's dependency on Breath-specific
hardware components so it can be shared across the Panda firmware family, and so the
board-agnostic core can later be extracted to its own repository already proven on
hardware. No API v2 behavior change.

> **Where this lands.** The RFC is recorded in **DragonVent**, because DragonVent is
> the consumer that surfaced the problem. The *implementation* belongs in
> **DragonBreath**, which owns the code and has the hardware, the HIL harness and the
> shipped 1.0 to prove the refactor against — see [Motivation](#motivation).

## Motivation

DragonVent (BIGTREETECH Panda Vent) is being built on DragonBreath's core rather than
on OpenVent's, because this core has the pieces OpenVent lacks — Bambu LAN MQTT and
the app-only install model. Five components vendored cleanly and build for the Vent's
target today: `pb_wifi`, `pb_moonraker`, `pb_bambu`, `pb_evlog`, `pb_source`.

Four did not, and they are the ones that matter most:

| Component | Blocked by (`REQUIRES`) |
|---|---|
| `pb_httpd` — web API **and OTA install** | `pb_heater`, `pb_ntc`, `pb_leds`, `pb_policy` |
| `pb_portal` — setup UI | `pb_httpd` (transitively the above) |
| `pb_ha` — Home Assistant MQTT | `pb_policy` |
| `pb_hil` — HIL harness | `pb_fan`, `pb_leds`, `pb_ntc`, `pb_policy`, `pb_buttons` |

So the entire firmware-update path is unreachable to any sibling product, purely
because it shares a translation unit with the heater dashboard handlers.

The intent is to **fix this here, in DragonBreath, before extracting the core.** This
repo has the hardware, the HIL harness, and a shipped 1.0 that must not regress — so
the refactor gets validated by the product that can actually prove it. Doing it in a
sibling first would mean refactoring a heater we cannot test against, and DragonBreath
would inherit an unvalidated rewrite.

## The coupling is smaller than it looks

Of the **15 routes** registered in `pb_httpd.c`, only **three** are device-specific:

| Device-agnostic (12) | Device-specific (3) |
|---|---|
| `/api/v2/` `info` `health` `logs` `events` `console` `token` `restart` `factory-reset` `boot-inactive` `heartbeat`, plus `/settings`, `/update` | `/api/v2/state` · `/api/v2/command` · `/api/v2/calibration` |

Every part of the OTA machinery — `/update`, `/boot-inactive`, `app_update`, slot
verification, rollback — is **already device-agnostic**. It is not entangled; it is
merely colocated in one 1285-line file.

Call sites from `pb_httpd.c` into Breath-specific components, by target:

- `pb_policy` — 17 distinct functions (dominated by `pb_policy_get_snapshot`, ×7)
- `pb_heater` — 8 (config getters/setters: max target, foldback, comms timeout)
- `pb_ntc` — 7 (smoothed temp, offset, Rref, status)
- `pb_leds` — 2 (enable/disable)

`pb_httpd.c` contains **zero HTML**. It is pure API. (The 134 lines of hand-built
HTML live in `pb_portal.c`; see PR 3 below.)

## The safety net is weaker than it looks

`tests/check_api_v2_contract.sh` reads like a contract test but is **66 lines of
`grep` over source files**. It asserts that the literal `"/api/v2/state"` appears
inside `pb_httpd.c`, and that specific JS fragments (`EventSource('/api/v2/events')`,
`l.owner!==actor`, `id="a-range"`) appear inside `www/app.html`. It never compiles or
runs anything.

Two consequences, both load-bearing for this RFC:

1. **It cannot catch a response regression.** Nothing compares actual JSON.
2. **It fails by construction under this refactor**, because it encodes the current
   *file layout* as the contract. Moving a route out of `pb_httpd.c` breaks it while
   telling us nothing about behavior.

Rewriting it is therefore **on the critical path**, not a follow-up.

What *is* reusable is the host-test harness: `tests/run_*_host_test.sh` compiles a
component against `tests/stubs/` with `cc -std=c11 -Wall -Wextra -Werror` and runs it
— no ESP-IDF, no hardware — and CI already runs six of them. Notably `tests/stubs/`
already contains `pb_heater.h`, `pb_ntc.h`, `pb_leds.h` and `pb_fan.h`: the exact
dependencies to be cut.

## Proposal: a device interface

The core owns transport, auth, SSE, OTA, events, and the event log. The product
supplies a vtable:

```c
typedef struct {
    const char *product;                                 // "DragonBreath"
    void  (*state_json)(cJSON *root);                    // -> /api/v2/state
    int   (*command)(const char *op, const cJSON *args); // <- /api/v2/command
    void  (*heartbeat)(void);                            // lease keepalive, optional
    const pb_cal_iface_t *calibration;                   // NULL => route returns 404
    const pb_ui_desc_t   *ui;                            // drives the SPA (PR 4)
} pb_device_t;

esp_err_t pb_httpd_register_device(const pb_device_t *dev);
```

`pb_heater`, `pb_ntc`, `pb_policy` and `pb_leds` leave `pb_httpd`'s `REQUIRES`
entirely. Optional capabilities are `NULL` — the Vent has no NTC calibration; it has
four motor groups and a kit auto-detect ADC instead.

A secondary benefit: a **mock device makes the HTTP layer unit-testable for the first
time.** Today it cannot be host-tested at all.

## Sequencing

Deliberately ordered so the risky change lands behind a real test, not in front of one.

**PR 1 — extract pure JSON builders.** Pull the response builders out of the
`esp_http_server` handlers into pure functions over a snapshot struct. No dependency
change, no behavior change. Add `tests/run_httpd_host_test.sh` in the existing style,
asserting **golden responses**. Rewrite `check_api_v2_contract.sh` to assert over real
responses rather than file contents. *This is the PR that creates the safety net, and
the seam that makes it testable is the same seam that decouples it.*

**PR 2 — introduce `pb_device`.** The pure builders become vtable members; the Breath
implementation moves out. `pb_httpd`'s `REQUIRES` drops the four hardware components.
PR 1's golden test proves responses are byte-identical, then gets re-pointed at a mock
device.

**PR 3 — asset pipeline.** Move the 134 lines of C-string HTML in `pb_portal.c`
(`/setup`, `/fw`, `/diag`, `/console`) into `www/*.html`, gzipped and embedded via the
`target_add_binary_data` pattern `app.html` already uses. Independent of the rest;
valuable on its own, since editing HTML inside C string literals is a standing tax.

**PR 4 — UI descriptor.** Have the device describe its panels/readouts/controls as
data, so one shared SPA renders every product and a new product ships without touching
the web build. Branding reduces to CSS custom properties, `<title>` and favicon,
substituted at build time.

**Decided: the descriptor is fetched at runtime, not embedded at build time.** The
whole point is one HTML for N products, and embedding the descriptor reintroduces a
per-product build artifact — which is the thing being eliminated. Runtime fetch is
what makes the claim actually hold.

The cost is one extra request on a constrained socket pool. Mitigations, in order of
preference: serve it from the **existing** `/api/v2/info` response rather than adding a
route (the SPA already fetches `info` at load, so this costs zero extra round trips);
failing that, a long `Cache-Control` with the app version as the cache key, since the
descriptor only changes when the firmware does.

Core extraction to its own repo follows, consumed via the **ESP-IDF component manager**
(`idf_component.yml` git dependency) — the same mechanism already used for
`espressif/mdns` and `espressif/esp_websocket_client`, with `dependencies.lock`
pinning. Not a submodule: that route was already tried (`external/OpenVent`,
`EXTRA_COMPONENT_DIRS`) and abandoned in favour of vendoring.

## Non-goals

- **No API v2 behavior change.** `docs/api-v2.md` is a published contract and the
  revert-to-stock path depends on `/update` semantics. PRs 1–2 must be a pure internal
  restructure with byte-identical responses.
- **No core repo extraction in this RFC.** It is the motivation, not the scope.
- **No JS build toolchain.** The build currently needs only ESP-IDF and `gzip` — the
  favicon is deliberately pre-rendered so no headless browser is required. PR 4 keeps
  that property; a static-site generator or bundler would put Node in the firmware
  build and tax every contributor and CI runner.

## Risks

- **The contract test rewrite is on the critical path** (see above). If PR 1 slips, the
  refactor proceeds without a net.
- **`/update` regressions are high-consequence**: a bad image path can strand a user's
  only route back to stock. OTA is untouched by design, but it shares the file being
  restructured.
- **The UI descriptor may not express the existing Breath dashboard.** Falsify early
  and cheaply: attempt to express the current dashboard's panels in the schema *before*
  committing to PR 4. Lease/actor semantics and the runtime `maximum_c` ceiling are the
  likely sticking points.
- **PR 2 touches a shipped 1.0.** HIL should run against a real board before merge.

## Open questions

1. Where does the Breath's `pb_device` implementation live — `main/`, or its own
   component (`pb_device_breath`)? A component keeps `main/` thin and is
   symmetric across products.
2. Does `/api/v2/calibration` generalize (a named-parameter capability) or stay an
   NTC-specific optional interface? Leaning optional-and-specific until a second
   product needs calibration.
3. Should `pb_hil` be decoupled in the same pass, or left Breath-only for now? It has
   the widest hardware dependency set and the least reuse pressure.

*Resolved:* the UI descriptor is **fetched at runtime**, ideally folded into the
existing `/api/v2/info` response so it costs no extra round trip. See PR 4.
