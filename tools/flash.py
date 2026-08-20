#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""DragonBreath installer/flasher for the BIGTREETECH Panda Breath (ESP32-C3).

*** READ THIS FIRST ***
Installing DragonBreath OVERWRITES THE ENTIRE FLASH and ERASES the stock firmware.
BIGTREETECH does not publish stock images, so THE FULL BACKUP THIS TOOL TAKES IS
THE ONLY WAY BACK TO STOCK. If you skip the backup or lose the backup file, THERE
IS NO GOING BACK. Store the backup somewhere safe (copy it off this machine).

Backs up the ENTIRE existing flash to a timestamped file BEFORE writing anything,
then flashes the DragonBreath build. The backup is a full 4 MB image, so you can
return to stock:

    # First-time install (back up stock, then flash DragonBreath):
    python3 tools/flash.py

    # Restore a previous backup (e.g. go back to stock):
    python3 tools/flash.py --restore backups/stock-YYYYmmdd-HHMMSS.bin

    # Reset config only (Wi-Fi/token/policy/calibration) — keep the firmware:
    python3 tools/flash.py --erase-nvs

The Panda Breath has no exposed native-USB (GPIO18 is the SSR), so flashing is
over the on-board CH340K USB-C UART bridge. Plug the board's USB-C into your PC;
esptool auto-detects the port (override with --port).

Requires esptool (`pip install esptool`, or the copy bundled with ESP-IDF).
On Windows use the `py` launcher, not `python`: `py -m pip install esptool` then
`py flash.py ...` — other apps' python.exe on PATH (Inkscape/GIMP/…) can otherwise
shadow the Python you installed esptool into. See docs/WINDOWS_RECOVERY.md.
Tested against a V1.0.1 board — verify your board revision first.
"""
import argparse
import datetime
import hashlib
import os
import subprocess
import sys

CHIP = "esp32c3"
FLASH_SIZE = 0x400000  # 4 MB
# NVS (config) partition — matches partitions.csv. Erasing only this wipes Wi-Fi
# credentials, control token, saved policy, and sensor calibration, forcing AP
# re-provisioning on next boot — WITHOUT touching the firmware. The minimal
# "reset my config" fix (same effect as the on-device Power+Auto reset combo).
NVS_OFFSET = 0x9000
NVS_SIZE   = 0x5000    # 20 KB
# DragonBreath image layout — matches partitions.csv (stock Panda layout: otadata
# @ 0xe000, app slot ota_0 @ 0x10000). This is the USB/recovery "factory" write.
IMAGES = [
    ("0x0",      "bootloader/bootloader.bin"),
    ("0x8000",   "partition_table/partition-table.bin"),
    ("0xe000",   "ota_data_initial.bin"),
    ("0x10000",  "dragonbreath.bin"),
]
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def esptool_cmd(port, baud, *args):
    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP]
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", str(baud), *args]
    return cmd


def run(cmd):
    print("  $ " + " ".join(cmd))
    return subprocess.call(cmd)


# Fast -> reliable. Poor USB-serial links (some laptops, long/cheap cables, certain
# CH340 adapters) corrupt bulk transfers at high baud, so an operation that fails is
# retried at the next slower rate automatically. --baud pins a single rate instead.
BAUD_LADDER = [460800, 230400, 115200]


def run_esptool(port, bauds, *args):
    """Run one esptool operation, dropping to a slower baud on failure. esptool
    read_flash/write_flash re-run cleanly (they overwrite the whole target), so a
    failed high-baud attempt is safe to retry slower. Returns (rc, used_baud)."""
    rc = 1
    for i, b in enumerate(bauds):
        if len(bauds) > 1:
            print(f"      [connection speed: {b} baud]")
        rc = run(esptool_cmd(port, b, *args))
        if rc == 0:
            return 0, b
        if i + 1 < len(bauds):
            print(f"      {b} baud failed — retrying at {bauds[i + 1]} baud "
                  f"(slower, more reliable on poor connections)…")
    return rc, bauds[-1]


def check_esptool():
    try:
        subprocess.check_output([sys.executable, "-m", "esptool", "version"],
                                stderr=subprocess.STDOUT)
        return True
    except Exception:
        print(f"ERROR: esptool not found for THIS Python:\n         {sys.executable}")
        print("       Install it into this exact interpreter:")
        print(f'         "{sys.executable}" -m pip install esptool')
        print("       On Windows, if that path is not your real Python (e.g. it points")
        print("       at Inkscape/GIMP/etc.), use the 'py' launcher for BOTH steps so")
        print("       pip and this script share one interpreter:")
        print("         py -m pip install esptool")
        print("         py flash.py ...")
        print("       (or run from an ESP-IDF environment: . ~/esp/esp-idf/export.sh)")
        return False


def confirm(prompt):
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except (EOFError, KeyboardInterrupt):
        print()
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def do_backup(port, bauds, backup_dir):
    """Returns (path, used_baud) on success, or (None, None) on failure."""
    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(backup_dir, f"stock-{stamp}.bin")
    print(f"\n[1/3] Backing up the full {FLASH_SIZE // (1024*1024)} MB flash -> {path}")
    print("      (this reads the WHOLE chip; ~1-2 min. Do not unplug.)")
    rc, baud = run_esptool(port, bauds, "read_flash", "0", hex(FLASH_SIZE), path)
    if rc != 0:
        print("ERROR: backup read failed at all baud rates — NOT flashing. Fix the "
              "connection (cable/port) and retry.")
        return None, None

    # 1) exact size, 2) not a blank/failed read (real image starts with the ESP
    # magic byte 0xE9), 3) contents actually match the chip (on-device hash via
    # esptool verify_flash, not just the file size).
    size = os.path.getsize(path) if os.path.exists(path) else 0
    if size != FLASH_SIZE:
        print(f"ERROR: backup is {size} bytes, expected exactly {FLASH_SIZE}. NOT flashing.")
        return None, None
    with open(path, "rb") as f:
        magic = f.read(1)
    if magic != b"\xe9":
        print(f"ERROR: backup does not start with the ESP image magic (0xE9); got "
              f"0x{magic.hex() or '??'}. The read looks blank/corrupt — NOT flashing.")
        return None, None
    print("      verifying backup against the chip (hash)...")
    if run(esptool_cmd(port, baud, "verify_flash", "0x0", path)) != 0:
        print("ERROR: backup failed hash verification against the chip — NOT flashing.")
        return None, None

    digest = sha256_file(path)
    print(f"      backup OK: {size} bytes, verified.")
    print(f"      SHA-256: {digest}")
    print("      Keep this file safe — it is the ONLY way back to stock.")
    return path, baud


def do_flash(port, bauds, build_dir):
    args = []
    for offset, rel in IMAGES:
        p = os.path.join(build_dir, rel)
        if not os.path.exists(p):
            print(f"ERROR: missing build artifact {p}")
            print("       Build first:  ./tools/idf-build.sh . esp32c3 build")
            return 1
        args += [offset, p]
    app = os.path.join(build_dir, "dragonbreath.bin")
    print(f"\n[3/3] Flashing DragonBreath from {build_dir}")
    print(f"      app image SHA-256: {sha256_file(app)}")
    rc, _ = run_esptool(port, bauds,
                        "--before", "default_reset", "--after", "hard_reset",
                        "write_flash", "--flash_mode", "dio",
                        "--flash_size", "4MB", "--flash_freq", "80m", *args)
    return rc


def do_restore(port, bauds, image):
    if not os.path.exists(image):
        print(f"ERROR: backup image not found: {image}")
        return 1
    size = os.path.getsize(image)
    # A full-chip restore must be exactly the flash size — a wrong-size file would
    # write a truncated/misaligned image and brick the board.
    if size != FLASH_SIZE:
        print(f"ERROR: {image} is {size} bytes; a full restore must be exactly "
              f"{FLASH_SIZE} bytes (4 MB). Refusing.")
        return 1
    with open(image, "rb") as f:
        if f.read(1) != b"\xe9":
            print("ERROR: image does not start with the ESP magic (0xE9) — not a "
                  "valid full-flash image. Refusing.")
            return 1
    print(f"\nRestoring full flash image {image}")
    print(f"  size: {size} bytes   SHA-256: {sha256_file(image)}")
    if not confirm("This OVERWRITES the entire chip. Continue?"):
        print("Aborted.")
        return 1
    rc, _ = run_esptool(port, bauds,
                        "--before", "default_reset", "--after", "hard_reset",
                        "write_flash", "--flash_size", "detect", "0x0", image)
    if rc != 0:
        return rc
    # write_flash already verifies the write inline ("Hash of data verified") before
    # the reset. Do NOT run a second verify_flash here: --after hard_reset boots the
    # restored firmware, which immediately rewrites its own NVS and PHY/RF-calibration
    # regions, so a full-chip re-read no longer matches the file (guaranteed with an
    # nvs-scrubbed backup) and reports a *false* "digest mismatch."
    print("  restore complete — write hash-verified; the device is rebooting into "
          "the restored firmware.")
    return 0


def do_erase_nvs(port, bauds):
    # Erase ONLY the NVS/config region — the firmware is untouched. This is the
    # USB equivalent of the on-device Power+Auto reset combo: it clears Wi-Fi
    # credentials, the control token, saved policy, and sensor calibration, so
    # the device comes back up unconfigured (its own AP for re-provisioning).
    print(f"\nErase config only: NVS region {hex(NVS_OFFSET)} + {hex(NVS_SIZE)} "
          f"({NVS_SIZE // 1024} KB).")
    print("  Clears Wi-Fi credentials, control token, saved policy, and sensor")
    print("  calibration. The firmware is left intact; the device re-provisions")
    print("  (its own AP) on next boot. Same effect as the Power+Auto reset combo.")
    if not confirm("Erase the config (NVS)?"):
        print("Aborted.")
        return 1
    rc, _ = run_esptool(port, bauds, "erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    if rc != 0:
        print("ERROR: NVS erase failed.")
        return rc
    print("  config erased — power-cycle the board; it will come up unconfigured.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="DragonBreath flasher (backs up stock first).")
    ap.add_argument("--port", help="serial port (default: esptool auto-detect)")
    ap.add_argument("--baud", type=int, default=None,
                    help="pin a single baud rate; default auto-falls-back "
                         f"{'->'.join(str(b) for b in BAUD_LADDER)} on failure")
    ap.add_argument("--build-dir", default=os.path.join(REPO_ROOT, "build"),
                    help="ESP-IDF build dir (default: <repo>/build)")
    ap.add_argument("--backup-dir", default=os.path.join(REPO_ROOT, "backups"),
                    help="where to write the stock backup (default: <repo>/backups)")
    ap.add_argument("--restore", metavar="IMAGE",
                    help="restore a full flash image (e.g. a prior backup) and exit")
    ap.add_argument("--backup-only", action="store_true",
                    help="take a verified full stock backup and exit (no flashing) — "
                         "the recommended safety net before a no-USB OTA install")
    ap.add_argument("--no-backup", action="store_true",
                    help="skip the pre-flash backup (NOT recommended)")
    ap.add_argument("--erase-nvs", action="store_true",
                    help="erase ONLY the NVS/config region (Wi-Fi, control token, "
                         "policy, calibration) and exit — a config reset that keeps "
                         "the firmware; the USB twin of the Power+Auto reset combo")
    args = ap.parse_args()

    if not check_esptool():
        return 1

    # Pinned rate -> use only that; otherwise the fast->reliable auto-fallback ladder.
    bauds = [args.baud] if args.baud else list(BAUD_LADDER)

    if args.restore:
        return do_restore(args.port, bauds, args.restore)

    if args.erase_nvs:
        return do_erase_nvs(args.port, bauds)

    if args.backup_only:
        path, _ = do_backup(args.port, bauds, args.backup_dir)
        if path is None:
            return 1
        print(f"\nStock backup saved: {path}")
        print("Keep it safe — copy it off this machine (cloud/USB). Restore with:")
        print(f"  python3 tools/flash.py --restore {path}")
        return 0

    print("=" * 70)
    print(" DragonBreath flasher")
    print(" !! THIS OVERWRITES THE WHOLE DEVICE AND ERASES THE STOCK FIRMWARE !!")
    print(" There is NO way back to stock without a full backup, and BIGTREETECH")
    print(" does not publish stock images. The backup taken below is your ONLY")
    print(" recovery path — keep it safe and copy it off this machine.")
    print("=" * 70)
    if args.no_backup:
        print("\n*** --no-backup: SKIPPING the stock backup. ***")
        print("*** If you do not ALREADY have a known-good backup stored safely, you")
        print("*** will PERMANENTLY lose the ability to return to stock firmware. ***")
        if not confirm("Really flash WITHOUT taking a backup?"):
            print("Aborted."); return 1
        if not confirm("Are you SURE? This cannot be undone without a backup."):
            print("Aborted."); return 1
    else:
        path, used_baud = do_backup(args.port, bauds, args.backup_dir)
        if path is None:
            return 1
        # The backup found a baud the link handles for a full 4 MB transfer; start
        # the flash there (and below) so we don't re-hit a known-bad higher rate.
        bauds = [b for b in bauds if b <= used_baud] or [used_baud]
        print(f"\n*** IMPORTANT: keep {path} safe — it is the ONLY way back to")
        print("*** stock. Copy it off this machine (cloud/USB) before continuing. ***")

    print("\n[2/3] Ready to flash DragonBreath.")
    if not confirm("Proceed with flashing?"):
        print("Aborted. Your backup (if taken) is kept.")
        return 1

    rc = do_flash(args.port, bauds, args.build_dir)
    if rc == 0:
        print("\nDone. The board reboots into DragonBreath. On first boot with no saved")
        print("Wi-Fi it starts an 'DragonBreath_XXXX' AP — connect and open http://192.168.4.1")
    else:
        print("\nFlash FAILED. If the board is unresponsive, restore your backup:")
        print("  python3 tools/flash.py --restore <backup.bin>")
    return rc


if __name__ == "__main__":
    sys.exit(main())
