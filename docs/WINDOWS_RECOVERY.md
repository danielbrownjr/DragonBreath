# Recovering / restoring DragonBreath on Windows (USB)

`tools/flash.py` backs up, restores, or unbricks a Panda Breath over its on-board
CH340 USB-C bridge. You normally **don't** need this — installs and updates use the
no-USB web OTA (upload `dragonbreath-<ver>.bin` from the web UI). Use `flash.py`
only to:

- take a **stock backup** before your first install (the only guaranteed way back),
- **go back to stock** from a backup, or
- **unbrick** a board that won't boot.

You do **not** need to clone the repo — `flash.py` is a single self-contained file.

## 1. Install Python + esptool

Install Python 3.13 (this also installs the `py` launcher):

```
winget install Python.Python.3.13
```

**Close the terminal and open a new one** (so PATH updates), then install esptool
**with the `py` launcher**:

```
py -m pip install esptool
```

> ### Use `py`, not `python` — this is the #1 Windows gotcha
> Other apps (Inkscape, GIMP, …) ship their own `python.exe` that can sit *ahead* of
> the Python you just installed on your PATH. When that happens, `pip` installs
> esptool into one Python while `python flash.py` runs a *different* one that can't
> find it — you get `esptool not found` even though `pip` says it's already
> installed, in an endless loop. The **`py` launcher** always targets your real
> registered Python, sidestepping this entirely. Use `py` for **both** the install
> and the script.

## 2. Get flash.py

Download just this one file (no repo clone needed):

```
https://raw.githubusercontent.com/plastikman/DragonBreath/main/tools/flash.py
```

Save it somewhere simple (e.g. your Downloads folder) and `cd` there, or pass its
full path.

## 3. Plug in the board

Plug the Panda Breath's USB-C into the PC. Windows 10/11 usually installs the CH340
driver automatically and a **COM port** appears; `flash.py` auto-detects it. If no
COM port shows up in Device Manager, install the **WCH CH340 driver**, then re-plug.
You can force a port with `--port COM7`.

## 4. Run it (always `py`)

Take a stock backup **before** your first DragonBreath install — keep the file safe,
it's your only way back:

```
py flash.py --backup-only
```

Go back to 100% stock from a backup:

```
py flash.py --restore path\to\your-stock-backup.bin
```

You'll see esptool write the 4 MB image and print **`Hash of data verified`** — that
is the write confirmed. The board then reboots into the restored firmware. (There is
no separate post-write verify: the firmware rewrites its own NVS/calibration on first
boot, so a re-read would falsely "mismatch" — the inline hash above is the real
check.)

## Reset the config only (keep the firmware)

If the device is booting fine but you're locked out — wrong Wi-Fi, a forgotten
control token, or a bad saved setting — you don't need a full restore. Erase just
the config (NVS) region and the device comes back up unconfigured, serving its own
setup AP again:

```
py flash.py --erase-nvs
```

This clears Wi-Fi credentials, the control token, saved policy, and sensor
calibration; the firmware itself is untouched. It's the USB equivalent of the
on-device **Power + Auto reset combo** (hold both front buttons for 5 seconds — all
panel LEDs flash 3× to confirm, then the board reboots erased).

## Notes

- **Always `py`, never `python`.** If `py` somehow isn't available, use the full path
  to your real Python, e.g.:
  ```
  & "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe" flash.py --restore ...
  ```
- `flash.py` needs only Python stdlib + esptool — nothing else from the repo.
- For a full USB *install* (not just backup/restore), download the release's
  `dragonbreath-<ver>-install-bundle.zip` instead — it bundles `flash.py` with all
  the firmware artifacts; unzip and run `py flash.py --build-dir .`.
