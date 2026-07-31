# RFC: Seamless no-USB migration — stock Panda Breath → DragonBreath (paxx 1.6.0)

Status: **Draft / design — for review.** paxx 1.6.0 (paxx12) merges DragonBreath and
**removes the stock-Panda chamber-heater integration entirely**. Any user still running
**stock Panda firmware** with the old "Panda Auto" config therefore has a **dead chamber
heater** the moment they update — unless 1.6.0 converts them. This RFC is how we convert
them **automatically, over the network (no USB), safely, with a guaranteed fall-back to
stock.**

## Why this is a forced conversion, not an offer

1.5.2-paxx12 shipped **both** paths (Panda Auto + DragonBreath) in the settings
dropdown. 1.6.0 deletes the Panda path (paxx will not maintain two integrations). So on
update, an existing stock-Panda user's chamber heater stops being controllable until the
device is on DragonBreath and the config is repointed. The conversion must run for them,
seamlessly, or their heater silently breaks — the worst outcome.

## Current state (ground truth, verified)

- **Stock Panda** is reached at mDNS **`PandaBreath.local`** over a **WebSocket `/ws`**
  API; paxx `bind-klipper`s it to the U1's Moonraker (`panda_breath_cli.py` →
  `{"printer":{"name":"Klipper","ip":…,"port":…}}`). "Panda Auto" = stock follows the
  bed via Moonraker.
- **DragonBreath** is reached at **`dragonbreath.local`** over **HTTP API v2**, driven by
  the **dragonbreath-klipper** module (`[heater_generic dragonbreath]`, M141/M191).
  Installs **over stock as an app-only OTA on the stock partition layout**; the stock app
  stays in the **inactive OTA slot** (revert via **boot-inactive**, no USB).
- **Stock NVS layout (RE'd):** namespace **`app_nvs`** (same one DragonBreath uses), with
  WiFi in a **blob `wifi_info`**, Moonraker in **`moonraker_info`**, plus
  `bambu_mqtt_info` / `ha_mqtt_info`, `hotspot_ip`, `ntc_bit`, `ui_info`. DragonBreath
  reads separate `ssid`/`password`/`mk_host`/`mk_port` keys → **this is exactly why WiFi
  did not carry over on the bench.** Same namespace, different keys.
- paxx already carries: `01-install-dragonbreath.sh`, `dragonbreath.cfg`, the stock
  `panda_breath*.cfg`, the settings dropdown, `panda_breath_cli.py`, and a built
  `…dragonbreath_upgrade.bin`.

## The three moving parts

The conversion touches all three, but the **linchpin is a small DragonBreath firmware
change** — without it the flash drops the device to AP mode and the "seamless" promise
dies.

### A. DragonBreath firmware — stock-NVS carry-over shim  *(the linchpin)*

On first boot, if DragonBreath's own `ssid`/`password` keys are **absent** but the stock
**`app_nvs/wifi_info`** blob is present, parse it and populate DragonBreath's keys;
likewise `moonraker_info` → `mk_host`/`mk_port`. Result: **the OTA flip rejoins the exact
same WiFi and Moonraker with zero re-provisioning.**

- Small RE task: the `wifi_info` and `moonraker_info` blob layouts (packed structs in
  `app_nvs`). We already have stock backups to derive them from.
- Safe + idempotent: migrate **only** when our own keys are absent (never clobber a user
  who set creds via `/setup`). One-shot; writes our keys, then normal boot.
- Rref is auto-detected from the GPIO19 strap, so `ntc_bit` is not needed.
- Ships in a tagged DragonBreath release; this is the version paxx pins.

### B. paxx — the conversion orchestrator (the bulk of the work)

A boot-time/one-shot service that runs the state machine:

1. **Detect + classify** the chamber heater on the LAN:
   - DragonBreath already? (`dragonbreath.local` → `GET /api/v2/info` `project=dragonbreath`) → **done, skip.**
   - Stock Panda? (`PandaBreath.local` → `/ws`, fw in the known list) → **convert.**
   - Nothing reachable? → mark **"chamber heater: needs attention"**, retry next boot; never silently drop it.
2. **Flash DragonBreath over the network.** POST the **bundled, pinned**
   `dragonbreath-<ver>.bin` to the stock firmware-update endpoint (the same call the stock
   web UI's Firmware Update uses — **must RE/confirm the exact HTTP/WS request**). Stock
   writes it to the inactive slot and reboots into it; the **stock app remains in the
   other slot** as the rollback. Bundle the image in the paxx rootfs so conversion needs
   **no internet**.
3. **Wait + verify.** Re-resolve `dragonbreath.local` (or the same DHCP IP — same MAC
   usually keeps the lease); confirm `GET /api/v2/info` reports `project=dragonbreath` and
   the pinned version. WiFi carried over via shim (A).
4. **Swap the Klipper config.** Replace the stock `panda_breath*.cfg` (WS bind) with
   `dragonbreath.cfg` (the module), pointed at the device; **migrate the user's settings**
   (chamber setpoint, AUTO threshold, filter/mode). Stage it — keep the old config until
   the new one is verified live.
5. **Reload Klipper** — restart the klippy process (not `FIRMWARE_RESTART`; the module is
   Python and only loads on a process restart — a known gotcha).
6. **Verify + finalize.** Confirm `[heater_generic dragonbreath]` loads and responds
   (temp reads, a no-op setpoint); then remove the staged stock config and mark the
   dropdown DragonBreath-only.

Properties: **idempotent + resumable** (re-derives state each boot, safe to retry), and
**consent-gated for the destructive step** — see UX.

### C. dragonbreath-klipper module — small assists

- A discovery/health probe the orchestrator can call (resolve device, verify API v2 up).
- The config paxx generates (host, optional token, heater name) with the migrated
  settings.

## UX — forced but not silent

Because Panda is gone, doing nothing = broken heater. But auto-flashing a user's device
firmware unprompted is too aggressive. Resolution: **a blocking one-time notice on first
boot after updating to 1.6.0**, only when a stock Panda is detected:

> *"Your chamber heater needs a one-time upgrade to DragonBreath (Panda support has been
> retired). This happens over WiFi — no USB, ~1–2 minutes — and automatically reverts to
> the stock firmware if anything goes wrong. [Upgrade now]"*

One tap → fully automated (steps B2–B6) with a progress indicator → done. If they defer,
the heater sits in a clear **"needs upgrade"** state (not silently dead), and the prompt
re-appears. If no device is present, nothing happens.

## Safety / rollback (non-negotiable — this is a forced flash)

- **Stock stays in the inactive OTA slot.** The stock bootloader + partition table are
  untouched (DragonBreath ships on the stock layout), so the escape hatch is always
  **boot-inactive → stock**.
- **Dual-OTA rollback:** a bad image that never marks itself healthy rolls back on the
  next boot automatically.
- **Staged config:** the stock Klipper config is kept until the DragonBreath heater is
  verified live; a failed verify reverts the config, not just the firmware.
- **No bricking path:** the flash never touches the bootloader; worst case the device
  reverts to stock and the orchestrator reports "conversion failed — heater on stock,
  retry?" rather than leaving a dead unit.
- **Fail loud, not silent:** every failure surfaces in the UI with a retry, because the
  alternative (a quietly non-working heater after a forced update) is unacceptable.

## Sequencing

1. **DragonBreath firmware:** RE the `wifi_info` + `moonraker_info` blob layouts, add the
   first-boot carry-over shim, cut a release. **Bench-prove the linchpin:** flash stock →
   OTA DragonBreath → confirm it rejoins WiFi **and** Moonraker with **no injection**.
2. **paxx:** RE/confirm the stock network firmware-update call; build the orchestrator
   (detect → flash → verify → config-swap → reload → verify), bundle the pinned image,
   wire the one-time prompt + "needs attention" state; delete the Panda path.
3. **dragonbreath-klipper:** discovery/health helper + the generated config.
4. **End-to-end on a real U1:** a stock-Panda user updates to 1.6.0 → one tap → DragonBreath,
   settings carried, no USB. Include the failure paths (device offline mid-flash; DHCP IP
   change; flash rejected) in the test matrix.

## Open questions / to verify

1. **The exact stock firmware-update network call** — HTTP `POST /ota` vs a `/ws` command.
   It provably exists (users OTA'd 1.0.0 via the stock web UI); pin the request shape
   (endpoint, headers, chunking) from the stock firmware.
2. **`wifi_info` / `moonraker_info` blob layouts** — small RE from the stock backups.
3. **DHCP IP stability** across the mDNS-name change (`PandaBreath.local` →
   `dragonbreath.local`) — same MAC usually keeps the lease; rely on mDNS + a fallback
   subnet probe by MAC/OUI.
4. **Settings mapping** — enumerate the stock settings the user may have and their
   DragonBreath equivalents (setpoint, AUTO threshold, filter). Carry all we can; default
   the rest.
5. **Consent model** — one-tap prompt (recommended) vs fully silent. Given it's a forced
   firmware flash, an explicit one-tap with a clear "reverts to stock on failure" is the
   right balance.
6. **Version pinning** — how paxx tracks/bumps the bundled DragonBreath image across
   releases (a pinned URL + checksum, image vendored in the rootfs).
