# RFC: Dragon\* firmware family — unification

Status: **Draft / design — for review (plastikman + @justinh-rahb).** Unify the BIGTREETECH
**Panda** open-firmware efforts under one **Dragon\*** family: a single board-agnostic
**shared core** consumed by product firmwares (DragonBreath, DragonVent), living in a
new shared GitHub org. This RFC is the umbrella; the
[HTTP device-interface decouple RFC](decouple-httpd-device-interface.md) is its
technical prerequisite.

## Decisions locked (plastikman + @justinh-rahb)

1. **Shared core = a new standalone repo with a family-neutral prefix**, and a
   **clean-break prefix rename across the board**: kill `pb_`/`pv_` (legacy Panda
   product branding) → **`dc_`/`dcore_`** (core), **`db_`** (DragonBreath), **`dv_`**
   (DragonVent). No aliases (mirrors the OpenBreath→DragonBreath rebrand).
2. **DragonVent is a fresh repo built on the extracted core** (not on OpenVent's core).
   OpenVent is archived with a pointer to DragonVent.
3. **A new shared GitHub org**, co-owned by plastikman + @justinh-rahb, is the home for the
   core, the products, **and the Klipper helper(s)** (`dragonbreath-klipper` moves in).
4. **Scope for this pass: DragonBreath + DragonVent only.** Other Panda\* products
   onboard later by supplying a device implementation + UI descriptor.

## Names — proposed, confirm before use

Placeholders below; the category is decided, the strings are not. Pick once, then
they're load-bearing (repo URLs, prefixes, `idf_component.yml`).

| Thing | Proposal | Notes |
|---|---|---|
| Shared GitHub org | **`dragon-fw`** | neutral, unbranded; holds core **+ products + the Klipper helper(s)** |
| Shared core repo | **`dragon-core`** | the board-agnostic ESP-IDF component set |
| Core component prefix | **`dc_`** (or `dcore_`) | `dragon-core`; replaces `pb_*`/`pv_*` on shared components |
| Product prefixes | Breath **`db_`** (was `pb_`), Vent **`dv_`** (was `pv_`) | **full rename — everything Dragon-branded, no legacy `pb_`/`pv_`** |

**Prefix scheme (decided): kill `pb_` and `pv_` outright.** Every component gets a
Dragon prefix — **`dc_`/`dcore_`** (shared core), **`db_`** (DragonBreath), **`dv_`**
(DragonVent). `pb_` = "Panda Breath" and `pv_` = "Panda Vent" are legacy product
branding; the family rename is a clean break with no aliases (mirrors the earlier
OpenBreath→DragonBreath rebrand). The core-prefix choice (`dc_` vs `dcore_`) is the one
string still open.

## Architecture — one core, N products

The **core** owns everything a Panda product shares; the **product** owns its board,
sensors, actuators, policy, and a device implementation that plugs into the core.

```
dragon-core (dc_*/dcore_*)                 DragonBreath repo (db_*)
  dc_httpd     transport/auth/SSE/OTA/       db_board    GPIO map
               events/log + dc_device vtable  db_ntc      sensors
  dc_portal    setup/fw/diag/console UI       db_heater   actuator + safety
  dc_wifi      wifi + captive portal          db_fan
  dc_moonraker Klipper bed state              db_policy   state machine
  dc_bambu     Bambu LAN MQTT                 db_leds / db_buttons
  dc_ha        Home Assistant MQTT            db_device   <- implements dc_device
  dc_source    control-source selector        main/       wires product -> core
  dc_evlog     event ring
                                             DragonVent repo (dv_*): dv_board,
                                             dv_motor…, dv_device -> same dc_ core
```

- **The seam is the `dc_device` vtable** from the decouple RFC: the product supplies
  `state_json` / `command` / optional `calibration` / a **UI descriptor**, and the core
  supplies transport, OTA, and the single shared SPA. DragonVent has no NTC calibration
  (returns `NULL`) and describes motor groups instead of a heater — same core, no forks.
- **Consumed via the ESP-IDF component manager** (`idf_component.yml` git dependency +
  `dependencies.lock` pin) — the mechanism already used for `espressif/mdns` and
  `espressif/esp_websocket_client`. **Not a submodule**: `external/OpenVent` +
  `EXTRA_COMPONENT_DIRS` was tried and abandoned in favour of vendoring; the manager is
  the version-pinned successor.
- **Branding is data, not code**: product name, `<title>`, favicon, and CSS custom
  properties are supplied by the product/descriptor, so one HTML renders every product.

## Sequencing

Ordered so nothing ships broken and the risky moves land behind a proven core. Steps
1–2 happen **in DragonBreath** (the only repo with hardware, HIL, and a shipped 1.0);
extraction and DragonVent follow.

**0. Stand up the family scaffolding.** Create the org, agree the names table above,
commit this RFC + the decouple RFC (done here). No code yet.

**1. Land the decouple RFC in DragonBreath** ([its PRs 1–4](decouple-httpd-device-interface.md)):
extract pure JSON builders + **golden-response contract test** (the safety net — the
current `check_api_v2_contract.sh` is grep-over-source and would pass a broken
refactor), introduce the `dc_device` vtable, move the HTML to an asset pipeline, add the
runtime UI descriptor (folded into `/api/v2/info`). **No API v2 behavior change**;
validated on real hardware + HIL. DragonBreath stays shippable throughout.

**2. Rename + extract `dragon-core`.** Two scripted renames in one step: the
board-agnostic set `pb_*` → `dc_*` (moves to the new repo, consumed via the component
manager) and the DragonBreath-specific set `pb_*` → `db_*` (stays in-repo). One
mechanical pass so review is "did the rename touch anything it shouldn't." Acceptance:
DragonBreath builds, host tests + HIL green, `/update` + revert unchanged — the core is
only proven once the shipped product still passes on it.

**3. Move everything into the org.** Transfer `DragonBreath` **and `dragonbreath-klipper`**
(GitHub redirects cover old links); update cross-references (`idf_component.yml`, READMEs,
Moonraker `[update_manager]` origins, `help_url`s). The Klipper helper lives in the
family org alongside the firmware.

**4. DragonVent — fresh repo on `dragon-core`.** New `DragonVent` repo; @justinh-rahb supplies
the Vent device implementation (`dv_*` board + motor groups + kit auto-detect ADC) and
its UI descriptor. Validate on Vent hardware. **Archive OpenVent** with a README pointer
to DragonVent (no history migration — the core lineage differs).

**5. (Later) Other Panda\* products** onboard by repeating step 4: a device impl + a UI
descriptor, no core fork.

## Division of labor

- **Core + DragonBreath refactor (steps 1–3): plastikman** — owns the code, the
  hardware, the HIL harness, and the shipped 1.0 the refactor must not regress.
- **DragonVent device impl (step 4): @justinh-rahb** — owns the Vent hardware and its behavior.
- **`dragon-core` interface + governance: joint** — both maintain; the `dc_device`
  contract changes need both products in mind.

## Risks

- **Contract-test rewrite is on the critical path** (from the decouple RFC). If it
  slips, the extraction proceeds without a net that can catch a JSON regression.
- **`/update` is high-consequence** — a bad image path strands a user's only route back
  to stock. OTA is untouched by design but shares the file being restructured; HIL a
  real board before merging step 2.
- **Prefix churn (`pb_*` → `dc_*`) is a large mechanical diff** — do it as one scripted
  rename in step 2, not smeared across PRs, so review is "did the rename touch anything
  it shouldn't" rather than reading every hunk.
- **Org transfer breaks external links** — GitHub redirects cover most; the klipper
  helper's `update_manager` origin and any `help_url`s need explicit updates.
- **Two owners on a shared core needs light governance** — agree upfront who can cut a
  `dragon-core` release and how products pin it (`dependencies.lock`).
- **The UI descriptor may not express both dashboards** — falsify early: express the
  Breath dashboard AND a sketch of the Vent panel in the schema *before* committing to
  descriptor-driven UI. Lease/actor semantics and the runtime `maximum_c` ceiling are
  the known sticking points for Breath; motor-group controls are the unknown for Vent.

## Non-goals

- **No API v2 behavior change** in the extraction — `docs/api-v2.md` is a published
  contract and revert-to-stock depends on `/update` semantics.
- **No JS build toolchain** — the build needs only ESP-IDF + `gzip`; the favicon is
  pre-rendered so no headless browser is required. Keep that for every product.
- **No rename of the "Panda Breath"/"Panda Vent" hardware descriptors** — those name
  BIQU's boards and stay as hardware descriptors only.

## Open questions

1. **Confirm the strings** — org name (`dragon-fw`?), core repo (`dragon-core`?), and
   the **core prefix `dc_` vs `dcore_`** (the one prefix still open; product prefixes
   `db_`/`dv_` are decided).
2. **Where does the Breath `dc_device` implementation live** — `main/`, or its own
   `db_device` component? (Decouple RFC leans component, for symmetry across products.)
   Same question for the Vent side (`dv_device`).
3. **Klipper helper structure — per-product or one shared helper?** It's decided the
   helper(s) move into the family org; open is whether DragonBreath's heater helper and
   any DragonVent fan/filter helper are **separate repos** or **one `dragon-klipper`**
   with per-product config. Leaning separate until the Vent actually needs a Klipper
   object. (`dragonbreath-klipper` also gets `pb_`→`db_` naming where it references the
   firmware's auth header / config keys — deploy lockstep, so rename together.)
