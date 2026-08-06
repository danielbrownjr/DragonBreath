# RFC: MQTT-only Klipper integration mode

Status: **Proposed.** This adds a portable integration alongside
[`dragonbreath-klipper`](https://github.com/plastikman/dragonbreath-klipper), not a
replacement for it. The existing helper remains the preferred integration whenever
the user can install Klipper extras; this mode serves managed or locked systems where
the user can edit Moonraker and macro configuration but cannot install an extra.

## Goal

Provide bidirectional DragonBreath control and telemetry using only an MQTT broker,
`moonraker.conf`, and `printer.cfg` macros. It must provide dashboard telemetry,
setpoint/mode control, macro-visible device state, and a Mainsail/Fluidd power toggle
without placing any files in `klippy/extras/`.

This mode is deliberately not a native Klipper heater. It does not provide
`verify_heater`, a native temperature object, or blocking `M191` semantics.

## Architecture

```
DragonBreath <---- MQTT ----> central broker <---- MQTT ----> Moonraker <----> Klipper
```

The integration has four separate data paths:

| Path | Transport | Consumer |
|---|---|---|
| Desired state | macro variables -> Moonraker `status_objects` -> MQTT | DragonBreath firmware |
| Telemetry | DragonBreath MQTT payload -> Moonraker `[sensor] type: mqtt` | Mainsail/Fluidd dashboard |
| Macro telemetry | DragonBreath MQTT JSON-RPC -> `printer.gcode.script` -> `SET_GCODE_VARIABLE` | Klipper macros |
| Master enable | Moonraker `[power] type: mqtt` | Mainsail/Fluidd and macros |

The dashboard sensor and the macro variable bridge intentionally carry the same
measurement twice. Moonraker's sensor state is not a Klipper `printer[...]` object,
so a macro cannot read it directly. The two paths can briefly disagree; macro logic
must never assume they are simultaneous.

## Central broker is required

A shared Mosquitto-compatible broker is a hard requirement. Moonraker supports one
`[mqtt]` section, so pointing it at an individual device's embedded broker does not
scale beyond one device. If DragonBreath ever exposes an embedded broker, it is for
setup or diagnostics only and must not be documented as this integration's broker.

## Moonraker configuration

```ini
[mqtt]
address: mosquitto.lan
enable_moonraker_api: True
instance_name: myprinter
publish_split_status: True
status_objects:
  gcode_macro DRAGONBREATH
  gcode_macro DB_LINK

[sensor dragonbreath]
type: mqtt
name: DragonBreath
state_topic: dragonbreath/telemetry
state_response_template:
  {% set state = payload|fromjson %}
  {set_result("chamber_temperature", state["chamber_temperature"]|float)}
  {set_result("element_temperature", state["element_temperature"]|float)}
  {set_result("humidity", state["humidity"]|float)}
parameter_chamber_temperature:
  units=C
parameter_element_temperature:
  units=C
parameter_humidity:
  units=%

[power dragonbreath]
type: mqtt
command_topic: dragonbreath/power/set
command_payload:
  {command}
state_topic: dragonbreath/power/state
state_response_template:
  {payload}
retain_command_state: False
query_after_command: False
```

With `publish_split_status: True`, Moonraker publishes each field separately under:

```
{instance}/klipper/state/{objectname}/{statename}
```

DragonBreath subscribes to:

```
myprinter/klipper/state/gcode_macro DRAGONBREATH/#
myprinter/klipper/state/gcode_macro DB_LINK/#
```

It caches the individual fields locally. This is intentionally not described as an
atomic snapshot.

## Desired-state contract

All persistent commands live on one macro object. A sequence value is written last;
the device applies its cached desired state only after observing a new sequence.
This avoids temporarily applying a new `mode` with the preceding `target` while
Moonraker publishes individual fields.

```ini
[gcode_macro DRAGONBREATH]
variable_seq: 0
variable_target: 0.0
variable_mode: "off"
variable_fan: 0
variable_armed: 0
variable_purge_nonce: 0
# Device-written telemetry for macro logic:
variable_temperature: -1.0
variable_humidity: -1.0
variable_fault: ""
gcode:
```

A macro changing desired state writes all relevant values first, then increments
`seq`. A one-shot action such as purge increments `purge_nonce`, rather than using a
boolean that cannot express a repeated request. `publish_mqtt_topic` may be used as
an optional, non-retained event channel on Moonraker versions that support it, but
it is not required for this mode's control plane.

## Heartbeat and arming

Moonraker publishes status only on change; a static target is not proof that Klippy
is still running. A second object provides an explicit liveness signal:

```ini
[gcode_macro DB_LINK]
variable_heartbeat: 0
gcode:

[delayed_gcode DB_HEARTBEAT]
initial_duration: 5
gcode:
  {% set hb = printer["gcode_macro DB_LINK"].heartbeat|int %}
  SET_GCODE_VARIABLE MACRO=DB_LINK VARIABLE=heartbeat VALUE={hb + 1}
  UPDATE_DELAYED_GCODE ID=DB_HEARTBEAT DURATION=5
```

The device uses broker MQTT keepalive for transport/broker loss and the incrementing
heartbeat for the Klippy/Moonraker desired-state path. The proposed cadence is 5 s;
three missed increments (15 s) force heat off and latch `comms_lost`.

Safety invariant:

> DragonBreath never energizes heat merely because it received desired state. It
> requires an explicit arm observed after a fresh heartbeat.

Boot, reconnect, retained delivery, and heartbeat recovery all begin disarmed.
Firmware must require an explicit new arm transition after recovery. Retained
messages may update a local display but must never arm heat; DragonBreath-published
application messages are always non-retained.

Thermal cutoffs, the hardware watchdog, maximum temperature, fail-safe airflow, and
all fail-off behavior remain inside DragonBreath firmware. This integration does not
weaken the existing device safety model.

## Device-to-macro writeback

For macro logic that needs live temperature or fault state, DragonBreath sends an
MQTT JSON-RPC request to Moonraker's API topic:

```json
{
  "jsonrpc": "2.0",
  "method": "printer.gcode.script",
  "params": {
    "script": "SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=temperature VALUE=42.3"
  },
  "id": 1
}
```

The request topic is `myprinter/moonraker/api/request`; responses arrive on
`myprinter/moonraker/api/response`. Device-to-Klipper writeback is limited to a
modest rate (proposed: every 2 s). It enters Klipper's G-code queue, so 250 ms
telemetry updates are inappropriate during a print.

## M141 supported; M191 deliberately unsupported

MQTT mode supports an `M141` shim that updates the desired target and sequence. It
does **not** provide a native blocking `M191`.

Klipper macros run to completion; they cannot yield for a future MQTT
`SET_GCODE_VARIABLE`. `delayed_gcode` is asynchronous, `TEMPERATURE_WAIT` needs a
real Klipper sensor, and `PAUSE`/`RESUME` collides with normal print pause/cancel
behavior. The MQTT-mode `M191` shim must raise this clear error instead of returning
success:

```
M191 unsupported in MQTT mode — use M141 in filament start G-code
```

## Broker security is mandatory

Enabling Moonraker's MQTT API lets any authorized publisher to the API request topic
invoke `printer.gcode.script`, which is arbitrary G-code execution. This is a real
security tradeoff: the native module is strictly safer because it does not expose a
network route to arbitrary G-code.

Documentation must ship a broker account and ACL example, not present ACLs as an
optional hardening step:

```
user dragonbreath
topic write dragonbreath/telemetry
topic write dragonbreath/power/state
topic write myprinter/moonraker/api/request
topic read  myprinter/moonraker/api/response
topic read  myprinter/klipper/state/gcode_macro DRAGONBREATH/#
topic read  myprinter/klipper/state/gcode_macro DB_LINK/#
```

In particular, do not grant `topic write myprinter/#`. ACLs do not constrain the
JSON-RPC method inside the permitted API request topic, so DragonBreath firmware is
a trusted printer-control actor. It must only use the small, documented set of API
calls needed for telemetry writeback and a conservatively gated fault escalation.

## Optional critical-fault escalation

After it has shut off its own heater, DragonBreath may call `printer.emergency_stop`
for a clearly defined, latching critical enclosure fault. This is optional and must
be disabled by default until its trigger policy is specified and tested. Ordinary
warnings and heater safety faults only report telemetry and leave the printer
running.

## Implementation phases

1. Define DragonBreath MQTT topic, payload, desired-state, arming, heartbeat, and
   fault schemas, including a version field.
2. Implement the MQTT control source in firmware using the existing safety and
   lease/policy boundaries; add host tests for reconnect, retained delivery,
   out-of-order fields, heartbeat timeout, and re-arm requirements.
3. Ship `moonraker.conf`, `printer.cfg`, and Mosquitto ACL examples plus a setup
   validator that refuses to enable the mode without a broker and credentials.
4. Validate on a stock/locked Klipper installation with Mainsail and Fluidd. Verify
   dashboard readings, state updates, queue behavior while printing, loss/recovery,
   and the proof that heat remains off until explicit re-arm.
5. Document MQTT mode as a compatibility path and retain the native module as the
   recommended path for native heater semantics and `M191`.

## Sources

- [Moonraker MQTT configuration](https://moonraker.readthedocs.io/en/stable/configuration/#mqtt)
- [Moonraker MQTT sensor configuration](https://moonraker.readthedocs.io/en/stable/configuration/#mqtt-sensor-configuration)
- [Moonraker printer API](https://moonraker.readthedocs.io/en/latest/external_api/printer/)
- [BTT Panda Sense Pro](https://neo.bttwiki.com/en/docs/panda-series/module/panda-sense-pro/#view-sensor-data)
- [iHeater Link Klipper setup](https://new.docs.idryer.org/en/projects/iheater/link/integrations/klipper-setup/)
- [OpenVent](https://github.com/justinh-rahb/OpenVent)
