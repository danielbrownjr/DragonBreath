# RFC: MQTT-only Klipper integration mode

Status: **Accepted specification.** The grounded implementation design and firmware
work are tracked in [#67](https://github.com/plastikman/DragonBreath/pull/67). This
revision incorporates the Moonraker/Klipper documentation findings recorded there and
the dragon-core extraction landed in #68.

## Goal and positioning

Provide bidirectional DragonBreath control and telemetry using a central MQTT broker,
`moonraker.conf`, and `printer.cfg` macros, without installing anything in
`klippy/extras/`.

This is a compatibility path for managed or locked Klipper systems. The native
[`dragonbreath-klipper`](https://github.com/plastikman/dragonbreath-klipper) extra
remains preferred wherever it can be installed because it provides native Klipper
heater semantics. MQTT mode deliberately does not provide `verify_heater`, a native
Klipper temperature object, or a truly blocking `M191`.

```text
DragonBreath <-> MQTT <-> central broker <-> Moonraker <-> Klipper
```

## Control-source and repository boundary

MQTT-Klipper is one value in the existing exactly-one-source selector. It is mutually
exclusive with Moonraker bed-follow, Bambu, Home Assistant, and unbound/manual mode.

After #68, the persisted selector is owned by dragon-core. The follow-on core change
must append:

```c
DC_SRC_KLIPPER_MQTT = 4
```

`DC_SRC_NONE` remains numeric value `3` for NVS compatibility, and an exclusive
`DC_SRC_MAX` bounds validation. Existing NVS namespace `app_nvs` and key `ctl_src`
remain unchanged.

The controller integration is DragonBreath product code and follows the `db_*`
convention (`db_klipper_mqtt`). Reusable MQTT parsing or transport may move to a
`dc_*` component later only if another product validates the boundary. Existing
product-local `pb_*` names are transitional, not the convention for new code.

## Verified Moonraker contract

The implementation depends on these researched wire facts:

- Moonraker availability is the retained `{instance}/moonraker/status` topic, also
  used for its last will, with `{"server":"online"}` or `{"server":"offline"}`.
- `publish_split_status: True` is required. Split status topics are retained and their
  payload is JSON shaped like `{"eventtime": ..., "value": ...}`, not a raw scalar.
- The combined `{instance}/klipper/status` topic is non-retained and is not the desired
  state transport.
- Object names retain their literal spaces in topic paths, for example
  `gcode_macro DRAGONBREATH`.
- Moonraker's MQTT API uses globally unique JSON-RPC IDs on a shared response topic.
  `printer.gcode.script` requests include `mqtt_timestamp` (or use QoS 0/2) to avoid
  duplicate execution.
- MQTT keepalive is fixed at 60 seconds and is not the heat-safety clock. The explicit
  five-second macro heartbeat and firmware dead-man timeout provide that safety.
- Moonraker `[sensor type: mqtt]` values appear in the web dashboard but are invisible
  to Klipper macros. Dashboard telemetry and optional macro writeback are separate
  paths.
- `SET_GCODE_VARIABLE` is in-memory only. Firmware restart/reconnect logic must
  republish desired state and reassert sequence rather than treating macro variables
  as durable storage.

## Topic map

`INST` is the required Moonraker `instance_name`. `DB` is the device topic base,
defaulting to `dragonbreath` and advanced-editable.

Device subscriptions:

| Topic | Purpose |
|---|---|
| `INST/klipper/state/gcode_macro DRAGONBREATH/#` | Retained desired-state fields |
| `INST/klipper/state/gcode_macro DB_LINK/#` | Retained heartbeat counter |
| `INST/moonraker/status` | Moonraker online/offline state |
| `INST/moonraker/api/response` | Correlated JSON-RPC responses |
| `DB/power/set` | Moonraker power-device master enable |

Device publications:

| Topic | Retained | Purpose |
|---|---:|---|
| `DB/telemetry` | No | Versioned temperatures, mode, target, fault, and `seq_ack` |
| `DB/power/state` | Yes | `on` / `off` state used to initialize Moonraker power UI |
| `DB/status` | Yes/LWT | Device `online` / `offline` availability |
| `INST/moonraker/api/request` | No | Optional macro-variable writeback |

No retained device publication is an arming input.

## Desired-state contract

Klipper exposes two macro objects through split status:

```ini
[gcode_macro DRAGONBREATH]
variable_seq: 0
variable_target: 0.0
variable_mode: "off"
variable_fan: 0
variable_armed: 0
variable_purge_nonce: 0
variable_temperature: -1.0
variable_humidity: -1.0
variable_fault: ""
gcode:

[gcode_macro DB_LINK]
variable_heartbeat: 0
gcode:
```

A command writes all desired fields first and increments `seq` last. Firmware applies
only a new sequence, preventing a partially published target/mode combination.
Repeatable one-shot operations increment a nonce rather than toggling a boolean.

A self-rescheduling delayed G-code increments `heartbeat` every five seconds. Three
missed increments (15 seconds) force heat off and latch `comms_lost`.

## Retained-aware arming invariant

> DragonBreath never energizes heat merely because desired state was delivered. Heat
> requires a coherent new sequence, a live post-connect arm edge, and a fresh
> heartbeat.

Each MQTT connection begins disarmed:

1. The first retained value for each desired-state and heartbeat field is recorded as
   the connection snapshot. Snapshot `armed=1` and snapshot heartbeat never prove
   liveness or permission to heat.
2. Liveness becomes true only after the heartbeat value changes post-connect and
   remains true while the last change is less than 15 seconds old.
3. Heat can arm only when `seq` advances after the coherent fields are present,
   `mode == "heat"`, a live `0 -> 1` arm edge was observed, and liveness is current.
4. While armed, each fresh heartbeat renews the DragonBreath policy lease and the
   hardware communications watchdog.
5. Disarm, mode-off, heartbeat timeout, MQTT disconnect, Moonraker offline, or master
   power-off immediately requests policy OFF. Recovery requires another live arm edge;
   retained `armed=1` never re-arms automatically.

Thermal cutoffs, maximum-temperature limits, sensor fail-closed behavior, airflow, and
hardware watchdogs remain authoritative inside DragonBreath firmware.

## Telemetry and macro writeback

`DB/telemetry` is non-retained, versioned JSON published on change or at a modest
cadence. It includes chamber and element temperatures, humidity where available,
mode, target, armed state, acknowledged sequence, and fault state. Moonraker's MQTT
sensor consumes it for Mainsail/Fluidd display.

Optional macro-visible writeback uses a correlated Moonraker JSON-RPC request:

```json
{
  "jsonrpc": "2.0",
  "method": "printer.gcode.script",
  "id": "globally-unique-id",
  "params": {
    "script": "SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=temperature VALUE=42.3",
    "mqtt_timestamp": 123456789
  }
}
```

Writeback is **off by default**, opt-in, rate-limited to roughly two seconds and
on-change only. It enters Klipper's G-code queue and must not contend with print moves.

## M141 and M191

Klipper has no native `M141` or `M191` commands in this configuration.

- `M141` is provided as a non-blocking shim that updates target state and sequence.
- A correct blocking `M191` is impossible without a real Klipper temperature sensor:
  macros expand to completion, cannot await a future MQTT update, and
  `TEMPERATURE_WAIT` cannot consume a Moonraker MQTT sensor.
- The `M191` shim is therefore a **non-blocking alias plus a visible warning**.
  This degrades gracefully instead of aborting prints whose start G-code emits M191.
- A hard-error variant was considered and rejected; generated configuration must not
  ship or recommend one.

## Broker and API security

A central Mosquitto-compatible broker is required. Moonraker supports one MQTT
section, so an embedded per-device broker is not the integration architecture.

Enabling Moonraker's MQTT API permits an authorized publisher to invoke
`printer.gcode.script`. Documentation and generated configuration must provide:

- a dedicated broker account and least-privilege ACL;
- no broad `write INST/#` grant;
- credentials, broker address, and `instance_name` validation before enabling the
  source;
- generated `moonraker.conf`, `printer.cfg`, and broker ACL from one saved setup;
- no password echo in generated or diagnostic output.

Optional `printer.emergency_stop` escalation is disabled by default until a narrow,
latching trigger policy is specified and tested. Ordinary DragonBreath faults shut off
the chamber and report telemetry without stopping the printer.

## Locked product decisions

1. Native `dragonbreath-klipper` remains the recommended integration.
2. `M191` is a non-blocking alias with a visible warning. Hard-error behavior is
   explicitly rejected.
3. Device topic base defaults to fixed `dragonbreath` and is advanced-editable.
4. Macro writeback is off by default.
5. Printer emergency-stop escalation is off by default.
6. MQTT-Klipper remains mutually exclusive with every other control source.

## Delivery and validation

1. **Wire/schema design — complete in #67.** The contract above is frozen for the
   initial implementation.
2. **Firmware and generated configuration — implemented on #67's pre-#68 branch.** Its
   retained-aware arming core has 26 host cases and its CI build passed.
3. **Post-extraction rebase — required before #67 merges.** Rebase onto #68, use
   `dc_source`, add the NVS-compatible enum value in dragon-core, adopt the app-side
   `db_klipper_mqtt` namespace, and refresh CI.
4. **Real locked-Klipper validation — required before release.** Test Mainsail and
   Fluidd telemetry, power control, target changes during a print, broker/Moonraker/
   Klippy loss and recovery, retained-message replay, explicit re-arm, API correlation,
   and queue behavior.
5. Document MQTT mode as a compatibility path; hardware validation gates its release
   tag, not acceptance of this RFC.

## Sources

- [Moonraker MQTT configuration](https://moonraker.readthedocs.io/en/stable/configuration/#mqtt)
- [Moonraker MQTT sensor configuration](https://moonraker.readthedocs.io/en/stable/configuration/#mqtt-sensor-configuration)
- [Moonraker printer API](https://moonraker.readthedocs.io/en/latest/external_api/printer/)
- [dragon-core](https://github.com/justinh-rahb/dragon-core)
- [BTT Panda Sense Pro](https://neo.bttwiki.com/en/docs/panda-series/module/panda-sense-pro/#view-sensor-data)
- [iHeater Link Klipper setup](https://new.docs.idryer.org/en/projects/iheater/link/integrations/klipper-setup/)
