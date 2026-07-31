# RFC: Dragon\* firmware family — unification

Status: **Draft / design — for review (plastikman + Justin).** Unify the BIGTREETECH
**Panda** open-firmware efforts under one **Dragon\*** family: a single board-agnostic
**shared core** consumed by product firmwares (DragonBreath, DragonVent), living in a
new shared GitHub org. This RFC is the umbrella; the
[HTTP device-interface decouple RFC](decouple-httpd-device-interface.md) is its
technical prerequisite.

## Decisions locked (plastikman + Justin)

1. **Shared core = a new standalone repo with a family-neutral component prefix.**
   `pb_*` ("Panda **B**reath") is product branding leaking into a board-agnostic core;
   the shared components move to a neutral prefix.
2. **DragonVent is a fresh repo built on the extracted core** (not on OpenVent's core).
   OpenVent is archived with a pointer to DragonVent.
3. **A new shared GitHub org**, co-owned by plastikman + Justin, is the home for the
   core and the products.
4. **Scope for this pass: DragonBreath + DragonVent only.** Other Panda\* products
   onboard later by supplying a device implementation + UI descriptor.

## Names — proposed, confirm before use

Placeholders below; the category is decided, the strings are not. Pick once, then
they're load-bearing (repo URLs, prefixes, `idf_component.yml`).

| Thing | Proposal | Notes |
|---|---|---|
| Shared GitHub org | **`dragon-fw`** | neutral, unbranded; holds core + products |
| Shared core repo | **`dragon-core`** | the board-agnostic ESP-IDF component set |
| Core component prefix | **`dc_`** | `dragon-core`; replaces `pb_*`/`pv_*` on shared components |
| Product prefixes | Breath **`pb_`** (kept — genuinely Panda Breath), Vent **`dv_`** | product-specific components stay product-prefixed |

## Architecture — one core, N products

The **core** owns everything a Panda product shares; the **product** owns its board,
sensors, actuators, policy, and a device implementation that plugs into the core.

```
dragon-core (dc_*)                         product repo (e.g. DragonBreath)
  dc_httpd     transport/auth/SSE/OTA/       pb_board    GPIO map
               events/log + dc_device vtable  pb_ntc      sensors
  dc_portal    setup/fw/diag/console UI       pb_heater   actuator + safety
  dc_wifi      wifi + captive portal          pb_fan
  dc_moonraker Klipper bed state              pb_policy   state machine
  dc_bambu     Bambu LAN MQTT                 pb_leds / pb_buttons
  dc_ha        Home Assistant MQTT            pb_device_breath  <- implements dc_device
  dc_source    control-source selector        main/       wires product -> core
  dc_evlog     event ring
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

**2. Extract `dragon-core`.** Rename the board-agnostic set `pb_*` → `dc_*`, move it to
the new repo, and have DragonBreath consume it via the component manager. Acceptance:
DragonBreath builds, host tests + HIL green, `/update` + revert unchanged — the core is
only proven once the shipped product still passes on it.

**3. Move the products into the org.** Transfer `DragonBreath` + `dragonbreath-klipper`
(GitHub redirects cover old links); update cross-references (`idf_component.yml`,
READMEs, `update_manager` origins).

**4. DragonVent — fresh repo on `dragon-core`.** New `DragonVent` repo; Justin supplies
the Vent device implementation (`dv_*` board + motor groups + kit auto-detect ADC) and
its UI descriptor. Validate on Vent hardware. **Archive OpenVent** with a README pointer
to DragonVent (no history migration — the core lineage differs).

**5. (Later) Other Panda\* products** onboard by repeating step 4: a device impl + a UI
descriptor, no core fork.

## Division of labor

- **Core + DragonBreath refactor (steps 1–3): plastikman** — owns the code, the
  hardware, the HIL harness, and the shipped 1.0 the refactor must not regress.
- **DragonVent device impl (step 4): Justin** — owns the Vent hardware and its behavior.
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

1. **Confirm the names** (org / core repo / `dc_` prefix / `dv_` for Vent).
2. **Where does the Breath `dc_device` implementation live** — `main/`, or its own
   `pb_device_breath` component? (Decouple RFC leans component, for symmetry across
   products.) Same question for the Vent side.
3. **Does the Klipper helper stay per-product?** DragonBreath has `dragonbreath-klipper`
   (a heater); DragonVent likely has no Klipper heater but may want a Moonraker-side
   fan/filter object — decide whether there's a shared helper pattern or per-product
   helpers.
4. **`dragonbreath-klipper` naming under the family** — rename to fit the org, or leave
   (it's product-specific, so arguably `pb_`-side and fine as-is).
