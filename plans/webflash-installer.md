# RFC: WiFi installer — no-USB DragonBreath onboarding via the stock updater

Status: **Draft / design — for review.** A way to install DragonBreath over WiFi
using the stock firmware's own web updater, with a device-generated stock backup as
the safety net, so a non-technical user never opens the unit or touches USB/esptool.

## Motivation

USB flashing is the single biggest barrier to adoption. It means opening the unit,
installing esptool, and running `tools/flash.py`. The **newer BIGTREETECH Panda
revision has no external USB at all**, so the current path requires physically
opening the case — a non-starter for most users ("our users are smart, but the
general population is not").

Every Panda already runs stock firmware with a built-in web updater. If we can push
DragonBreath *through that*, onboarding becomes: open a web page, upload one file,
done.

## What the reverse-engineering established (from the 1.0.3 stock image)

- **Stock has a local-file web OTA.** Its web UI (`http://PandaBreathe.local`) has a
  "Firmware Update" page: `ota_btn_click('.bin', 'ota_fw', 0x480000)` →
  `ota_post_file(file, id)` POSTs a **local `.bin`** which is written via standard
  ESP-IDF `esp_ota_ops` to the inactive app slot, then the device reboots. (There's
  also a separate `ota_img` target for an asset/IMG partition — irrelevant to us.)
- **No secure boot, no flash encryption.** Proven empirically: we read the full flash
  and wrote an unsigned DragonBreath image that boots. So an app we build is loadable.
- **No image-identity gate.** The only OTA validation strings are the stock
  `esp_image` ones (`invalid magic byte (expected 0xE9)`, `mismatch chip ID`, image
  checksum, partition-table MD5). There is **no "must be a Panda image" project-name
  check** — a valid ESP32-C3 app we build is accepted.
- **Stock partition table (4 MB, dual-OTA):**

  | Name | Type | Offset | Size |
  |---|---|---|---|
  | nvs | data/nvs | 0x9000 | 20 K |
  | otadata | data/ota | 0xe000 | 8 K |
  | **app0 / ota_0** | app | **0x10000** | **1920 K** |
  | **app1 / ota_1** | app | **0x1f0000** | **1920 K** |
  | spiffs | data/spiffs | 0x3d0000 | 188 K |
  | coredump | data/coredump | 0x3ff000 | 4 K |

  Each app slot is **1920 K** — DragonBreath's app (~1.05 MB) and a small installer
  both fit with room to spare.

## Hard constraint: we never ship Panda firmware

Stock Panda images are BIGTREETECH's, not ours. Therefore:
- **We distribute only DragonBreath-authored artifacts** — the installer app and the
  DragonBreath app. No BIQU/BTT bytes in anything we ship.
- **The stock backup is produced by the user's own device from its own flash** — the
  user backing up their own unit for their own recovery, never redistributed by us.
- **`pb_full.bin` / `app0.bin` / the `panda70c` RE artifacts are local-only** — used
  for RE and bench testing; never committed, released, or shipped.
- Matching the stock **partition layout** for interop is fine (a layout is not their
  firmware).

## The problem this solves

Two mechanisms are in play today:
- **Stock web OTA** rewrites only an **app slot** (leaves bootloader, partition table,
  the other slot, NVS intact).
- **Our USB flasher** clobbers the whole 4 MB *and* is the only thing that takes a
  backup.

A naive no-USB full-flash would be a kamikaze: no backup, no way back to Panda. The
installer fixes that by taking the backup **on-device, over WiFi, before** anything is
overwritten.

## Design

A small **DragonBreath Installer** — a standalone app we build and ship — that the
user flashes *through stock's own updater*, then drives from a web page.

### The installer app
- **Built for the stock partition table** (app slots at 0x10000 / 0x1f0000, 1920 K;
  otadata 0xe000) so the **stock bootloader** launches it after the stock OTA writes
  it to the inactive slot.
- **AP-only.** It can't read stock's WiFi credentials (different NVS schema), so it
  brings up its own AP (`DragonBreath-Installer`); the user connects directly and
  opens `192.168.4.1`. No home-WiFi provisioning needed — dead simple.
- **Self-contained.** Embeds the DragonBreath app image as a build asset (installer
  code ~0.4–0.8 MB + embedded DragonBreath ~1.05 MB ≈ well under the 1920 K slot), so
  the user uploads **one file** and never handles a second image.
- Only DragonBreath-authored code + our own firmware embedded. No Panda bytes.

### Stage 0 — get the installer on (via stock)
User: `PandaBreathe.local` → Firmware Update → upload `dragonbreath-installer.bin`.
Stock `esp_ota` writes it to the inactive slot and boots it. (One-time; uses the
tool BIQU already ships.)

### Stage 1 — back up stock over WiFi (read-only, zero risk)
Installer serves a page with **"Download stock backup"**: it reads the entire 4 MB via
`esp_flash_read` and streams it to the browser (`dragonbreath-panda-backup-<date>.bin`).
The user saves it. Nothing is written; this is the safety net.
- Caveat: since stock had to OTA the installer into one app slot, that slot now holds
  the installer, not the original stock app. The **other** app slot + bootloader +
  partition table + NVS are pristine, so Panda remains recoverable from the backup.

### Stage 2 — install DragonBreath (Option A — recommended)
Installer writes the embedded DragonBreath app to the **inactive** slot (the one it is
**not** running from — no self-overwrite), sets `otadata` to boot it, and reboots.
- **Only app slots + otadata are touched. The stock bootloader and partition table are
  never rewritten.** This is ordinary, well-trodden OTA behavior → rock-solid.
- Result: DragonBreath runs as a **stock-layout app** under the stock bootloader.
  Functionally identical; it's just a build variant pinned to the stock partition
  table, and its future OTAs use that layout.

### Reversibility / WiFi restore (a bonus of Option A)
Because Option A never touches the stock bootloader or partition table, **restore is
also a safe app-OTA**: DragonBreath (or the installer) can accept the user's
previously-downloaded stock backup, write the stock **app slot** from it, point
otadata back, and reboot into Panda — no self-overwrite, no USB, and no Panda IP
shipped by us (it's the user's own backup). This closes the full round-trip
(Panda → DragonBreath → Panda) over WiFi.

### Option B — full native-layout convert (not recommended for v1)
Rewrite bootloader + partition table + app to DragonBreath's *native* layout. This
overwrites the installer's own running slot, so it needs the flash-writer + reboot
pinned in IRAM and a careful write order, and a mid-write interruption **bricks** the
unit (the Stage-1 backup is the only recourse, and recovery would still need USB).
Only worth it if we ever require the native layout without USB. Keep USB as the
power-user native path instead.

## Build & packaging
- New build target `installer/` (or a Kconfig-selected build) producing
  `dragonbreath-installer.bin`, built against a **stock-layout partition CSV**
  (app0 0x10000/1920K, app1 0x1f0000/1920K, otadata 0xe000, nvs 0x9000).
- A **stock-layout DragonBreath app** (same partition CSV) embedded into the installer
  — distinct from the native-layout image the USB flasher uses.
- Release artifacts add `dragonbreath-installer.bin` (ours). USB flasher + native
  image stay as-is for the power-user path. Nothing Panda-derived is ever included.

## Risks & open questions
- **Stock-bootloader ↔ our-app compatibility.** Our app (ESP-IDF 5.3.5) must boot
  under stock's (unknown-version) bootloader. Usually fine (the bootloader just loads
  the image per its header), but this is the #1 thing to verify on hardware.
- **NVS coexistence.** We reuse the stock `nvs`/`spiffs` partitions; DragonBreath uses
  its own `app_nvs` namespace, so no collision, but confirm a first boot with a
  stock-populated NVS is clean (ignore unknown keys).
- **Slot occupancy.** Only two app slots and the installer needs one, so stock can't
  stay resident alongside DragonBreath — the stock app slot is overwritten at install
  (the downloaded backup is the safety net). Acceptable.
- **Backup completeness.** The download reflects flash *after* the installer took a
  slot (one slot = installer). Fine for recovering Panda; not a pristine dual-stock
  image. Note it in the UI.
- **`ota_fw` size cap** in stock's JS is `0x480000` (4.5 MB) — our installer (~1.5 MB)
  is well under.
- **Does stock's `/ota` endpoint accept a scripted POST** (for a future one-click host
  tool) or only its own JS? Nice-to-have, not required.

## Testing plan
- Restore `pb_full.bin` (1.0.3) to the bench over USB → a real stock unit to test on
  (local only; never shipped).
- Verify the chain: stock web-OTA the installer → installer boots (bootloader-compat
  check) → download backup → install DragonBreath → boots and runs → restore the
  backup → Panda boots again.
- Confirm free-heap headroom for the 4 MB streaming read on the installer.

## Out of scope
- Shipping, hosting, or embedding any Panda image (hard line).
- Option B (native convert) for v1.
- A signed/verified installer chain (nice later; stock itself is unsigned).

## Sources
Stock RE from the 1.0.3 image (`~/claude_scratch/dragon/pb_full.bin`, local only):
partition table decoded with `gen_esp32part.py`; web-OTA mechanism + validation +
absence of an identity gate from `strings` on the stock app; no-secure-boot confirmed
empirically by the existing USB flash/boot.
