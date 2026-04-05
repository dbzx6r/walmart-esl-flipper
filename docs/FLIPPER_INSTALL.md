# Installing the ESL Tool on Flipper Zero

This guide walks you through everything needed to run the ESL Tool FAP on your Flipper Zero — including preparing the Flipper itself.

---

## Overview

Two things need to be set up:

1. **Flipper Zero firmware** — community firmware (Unleashed or Momentum) is required for BLE Central mode.  The official firmware does not expose the APIs needed.
2. **ESL Tool FAP** — the actual app, installed on the SD card.

> If you just want to control tags from a PC/Mac/Linux without a Flipper, skip this entirely and use the [Python companion](../companion/esl_companion.py) — it works with no Flipper at all.

---

## Part 1 — Install Community Firmware on the Flipper

> Skip this if you already have Unleashed or Momentum firmware installed.

### Option A — Unleashed Firmware (recommended)

1. Download the latest release from:  
   **https://github.com/DarkFlippers/unleashed-firmware/releases**  
   Get the `.tgz` package (e.g., `unleashed-firmware-unlshd-080.tgz`).

2. Extract the archive — you'll see a `firmware.dfu` and an `update` folder.

3. Copy the entire extracted folder to your Flipper's SD card under:  
   `SD:/update/`

4. On the Flipper, navigate to: **Files → SD → update → (your folder) → Update**  
   Confirm the update — the Flipper will reboot into the new firmware.

5. After reboot the Flipper will show the Unleashed logo.  Your apps and settings are preserved.

Alternatively, use **qFlipper** (the desktop app):
- Connect Flipper via USB → click **Install from file** → select the `.dfu` file.

### Option B — Momentum Firmware

Same process, different release page:  
**https://github.com/Next-Flip/Momentum-Firmware/releases**

---

## Part 2 — Install the ESL Tool FAP

Choose the method that works best for you.

### Method 1 — uFBT (build from source, easiest)

[uFBT](https://github.com/flipperdevices/flipperzero-ufbt) is the official Flipper build tool.  It downloads the SDK automatically.

```bash
# Install uFBT (requires Python 3.8+)
pip install ufbt

# Connect your Flipper via USB, then:
cd flipper-app
ufbt update          # downloads the matching SDK for your Flipper firmware version
ufbt                 # builds the FAP → creates build/esl_tool.fap
ufbt launch          # installs + launches on connected Flipper
```

That's it.  The FAP installs to `Apps/Tools/ESL Tool` on the Flipper.

#### Rebuilding after code changes

```bash
cd flipper-app
ufbt        # rebuilds
ufbt launch # pushes to Flipper
```

---

### Method 2 — Copy pre-built .fap via SD card

If you have a pre-built `esl_tool.fap` (e.g., downloaded from the Releases page):

1. Insert the Flipper's microSD into a card reader.
2. Copy `esl_tool.fap` to:  
   `SD:/apps/Tools/esl_tool.fap`
3. Reinsert the SD card.
4. On the Flipper: **Apps → Tools → ESL Tool**

---

### Method 3 — Manual SDK build

Use this if you want to build against the full firmware SDK (e.g., to make modifications).

```bash
# Clone the Unleashed firmware (large — ~2 GB)
git clone https://github.com/DarkFlippers/unleashed-firmware.git

# Copy the app into the firmware tree
cp -r flipper-app/ unleashed-firmware/applications_user/esl_tool/

# Build the FAP
cd unleashed-firmware
./fbt fap_esl_tool

# The .fap will be at:
# build/f7-firmware-D/apps/Tools/esl_tool.fap
```

Copy the resulting `.fap` to `SD:/apps/Tools/` on the Flipper.

---

## Part 3 — Using the App

1. On the Flipper: **Apps → Tools → ESL Tool**
2. Select **Scan for ESL Tags**
3. The Flipper scans for ~10 seconds — tags advertising as `ESL_XXXXXX` will appear in the list
4. Select a tag → choose **Set Price** or **Clear Display**
5. Type the price (e.g., `$12.99`) and optional label (e.g., `Great Value`)
6. The app connects, uploads the image, and the tag refreshes in ~4 seconds

> The tags must already have ATC_TLSR_Paper firmware installed — see [FLASHING.md](FLASHING.md).

---

## Bonus: Use Your Flipper Zero as the UART Flasher

You don't need a separate CH340/CP2102 USB-Serial adapter to flash the tags.  Your Flipper Zero's GPIO pins can act as a UART bridge — this lets you flash the ATC firmware using just the Flipper.

### How it works

The Flipper Zero has a GPIO header with UART pins.  Using the **GPIO → USB-UART Bridge** mode, the Flipper becomes a USB-Serial adapter that the WebSerial flasher can connect to.

### Wiring (Flipper GPIO → Tag PCB)

```
Flipper GPIO Pin       Tag PCB Pad
─────────────────────────────────────
Pin 1  (3.3V out)  →  VCC
Pin 8  (GND)       →  GND
Pin 13 (TX / C3)   →  RX  (tag receive)
Pin 14 (RX / C4)   →  TX  (tag transmit)
```

> ⚠️ The Flipper's GPIO is 3.3V — this is correct and safe.  Never use 5V.

### Steps

1. Wire the Flipper GPIO to the tag pads (see wiring table above and [WIRING.md](WIRING.md)).
2. On the Flipper, open: **GPIO → USB-UART Bridge**  
   Set baud rate to **115200**, pin to **TX/RX (13/14)**.
3. Connect the Flipper to your PC via USB.
4. Open Chrome/Edge and navigate to the WebSerial flasher:  
   **https://atc1441.github.io/ATC_TLSR_Paper_UART_Flasher.html**
5. Click **Connect** — select the Flipper's serial port (appears as a USB CDC device).
6. Proceed with the normal [FLASHING.md](FLASHING.md) steps (Unlock Flash → Write Firmware).

This means your entire setup is:
- Flipper Zero (UART flasher + BLE controller)
- Four wires + soldering iron
- A Chrome/Edge browser

No separate USB-Serial adapter required.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `ufbt` says SDK mismatch | Run `ufbt update` to re-download the correct SDK version |
| FAP doesn't appear in app list | Check the `.fap` is in `SD:/apps/Tools/`, reboot Flipper |
| BLE scan finds nothing | Make sure tags have ATC firmware; Unleashed/Momentum required for scanning |
| `ufbt launch` fails "no device" | Connect Flipper via USB; unlock the Flipper (enter PIN) |
| Flipper UART bridge not detected | Try a different USB cable (data cable, not charge-only) |
| WebSerial won't see Flipper | Install Flipper USB drivers from qFlipper; use Chrome not Firefox |
