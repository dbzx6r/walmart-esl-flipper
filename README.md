# ESL Flipper Zero Tool

Repurpose Walmart / retail Hanshow Stellar e-ink price tags using a Flipper Zero or a PC.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Flipper%20Zero%20%7C%20Python-green)

---

## Quick Decision Guide

### I have SES-imagotag / Vusion HRD3-series tags

No firmware flashing needed! Use the `vusion` commands:

```bash
cd companion && pip install -r requirements.txt
python esl_companion.py vusion scan
python esl_companion.py vusion provision AA:BB:CC:DD:EE:FF
python esl_companion.py vusion display  AA:BB:CC:DD:EE:FF --image-index 0
```

Full guide: [docs/VUSION_ESL.md](docs/VUSION_ESL.md)

---

### I have Hanshow Stellar tags

**Do I need to flash the tags?**  
→ **Yes** — always. The stock Hanshow firmware uses a proprietary encrypted protocol that only works with Hanshow's commercial base stations. Custom firmware ([ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)) must be flashed once per tag via UART. Takes ~2 min per tag. See [docs/FLASHING.md](docs/FLASHING.md).

> **Already on eBay with custom firmware?** Many lots come pre-flashed — scan with nRF Connect; if you see a device named `ESL_XXXXXX`, skip flashing.

**Do I need a Flipper Zero?**  
→ **No** — you can control tags from any PC/Mac/Linux using the Python companion. Pick the path that works for you:

| Path | What you need | Guide |
|------|--------------|-------|
| 🐍 **Python only** | PC + Bluetooth adapter | [Quick Start → Python](#quick-start) |
| 🐬 **Flipper Zero FAP** | Flipper with Unleashed/Momentum firmware | [Flipper Installation](docs/FLIPPER_INSTALL.md) |
| 🌐 **Browser only** | Chrome/Edge + Bluetooth | [ATC WebBluetooth uploader](https://atc1441.github.io/ATC_TLSR_Paper_Image_Upload.html) |

> The Flipper Zero FAP requires **community firmware** (Unleashed or Momentum) — official firmware does not expose BLE Central APIs. The Python companion has no such requirement.

---

## What This Does

Electronic Shelf Labels (ESLs) — the small e-ink price tags found in Walmart and other retailers — can be repurposed using this tool.  Two tag families are supported:

### Hanshow Stellar tags (ATC_TLSR_Paper firmware)
After a one-time UART flash, they expose a simple BLE GATT interface that lets you:
- Display any **custom image** (250×122 px, 1-bit B&W)
- Show a **price tag layout** with large price text and an optional label line
- **Clear** the display to white

### SES-imagotag Vusion HRD3-series (BT SIG ESL Service)
No firmware flashing required — these use the **Qualcomm QCC710** chip and implement
the **Bluetooth SIG Electronic Shelf Label Service** (UUID `0x184D`):
- Display pre-stored factory images by cycling image slots
- Upload custom images via Object Transfer Protocol (Linux/BlueZ)

---

## Compatible Tags

### Hanshow Stellar (requires firmware flash)

Tags with an **"N" in the model name** use the compatible TLSR8359 SoC:

| Model | Display | Size |
|-------|---------|------|
| Stellar-MN@ E31H | 250×122 B&W | 2.13" |
| Stellar-M3N@ E31HA | 250×122 B&W/Red | 2.13" |
| Stellar-MFN@ E31A | 212×104 B&W | 2.13" alt |
| Stellar-S3TN@ E31HA | 200×200 B&W/Red | 1.54" |

> ⚠️ **Models WITHOUT "N"** (e.g. Stellar-M@) use a different SoC and are **not compatible**.

### SES-imagotag Vusion HRD3-series (no flash needed ✅)

| Model | Chip | Protocol |
|-------|------|---------|
| HRD3-0210-A | Qualcomm QCC710 | BT SIG ESL Service 0x184D |
| Other HRD3-XXXX | Qualcomm QCC710 | BT SIG ESL Service 0x184D |

> See [docs/VUSION_ESL.md](docs/VUSION_ESL.md) for the full Vusion guide.  
> See [docs/HARDWARE.md](docs/HARDWARE.md) for full identification guide.

---

## Prerequisites

### Step 1 — Flash the custom firmware on each tag (one-time, required)

The stock firmware uses a proprietary protocol.  You must replace it with ATC_TLSR_Paper via UART:

1. See [docs/WIRING.md](docs/WIRING.md) for wiring — works with a CH340 adapter **or your Flipper Zero's GPIO pins**.
2. Follow [docs/FLASHING.md](docs/FLASHING.md) step-by-step.
3. After flashing, the tag advertises as `ESL_XXXXXX` over Bluetooth.

### Step 2 — Choose your controller

#### Option A: Flipper Zero (community firmware required)

- Install **[Unleashed Firmware](https://github.com/DarkFlippers/unleashed-firmware)** or **Momentum Firmware** on your Flipper Zero.  The official firmware does not expose the BLE GATT Central APIs needed.
- Build or install the FAP — see [Flipper App Installation](#flipper-app-installation) below.

#### Option B: Python (any platform)

```bash
cd companion
pip install -r requirements.txt
```

Python requires Bluetooth hardware and a supported OS (Windows 10+, macOS 10.15+, Linux with BlueZ 5.43+).

---

## Quick Start

### Python companion

```bash
cd companion

# Scan for nearby ESL tags
python esl_companion.py scan

# Set a price ($12.99) with a label
python esl_companion.py price "AA:BB:CC:DD:EE:FF" '$12.99' --label "Great Value" --model BW213

# Upload a custom image
python esl_companion.py image "AA:BB:CC:DD:EE:FF" photo.jpg --model BW213

# Clear the display (all white)
python esl_companion.py clear "AA:BB:CC:DD:EE:FF"
```

Replace `AA:BB:CC:DD:EE:FF` with your tag's MAC address from the scan output.

### Image converter (standalone)

```bash
cd tools
python esl_image.py input.jpg output.bin --model BW213 --preview
```

The `output.bin` file can then be uploaded with `esl_companion.py image`.

---

## Flipper App Installation

For the full step-by-step guide (including how to install Unleashed firmware and how to use the Flipper as a UART adapter for tag flashing), see:

**[docs/FLIPPER_INSTALL.md](docs/FLIPPER_INSTALL.md)**

### Quick summary (uFBT method)

```bash
pip install ufbt          # install build tool
cd flipper-app
ufbt update               # download matching SDK
ufbt                      # build the FAP
ufbt launch               # install + launch on connected Flipper
```

### Usage on Flipper

1. Open **Apps → Tools → ESL Tool**
2. Select **Scan for ESL Tags** — the Flipper will scan for 10 seconds.
3. Select a found device from the list.
4. Choose **Set Price** or **Clear Display**.
5. Enter the price string (e.g. `$12.99`) and optional label.
6. The app connects and uploads — the display updates in ~4 seconds.

> **Note**: The Flipper Zero FAP uses the BLE GATT Central API available in community firmware.  If you're on official firmware, use the Python companion script instead.

---

## Project Structure

```
walmart-esl-flipper/
├── flipper-app/             # Flipper Zero FAP (C)
│   ├── application.fam      # FAP manifest (appid, icon, SDK version)
│   ├── esl_app.c/h          # Entry point + scene manager
│   ├── esl_ble.c/h          # BLE scan + GATT client layer
│   ├── esl_protocol.c/h     # Image encoding + command protocol
│   └── esl_ui.c/h           # Scene-based UI (menus, price entry, etc.)
├── companion/               # Python BLE companion (PC/Mac/Linux/RPi)
│   ├── esl_companion.py     # CLI: scan, price, image, clear, serial
│   ├── image_converter.py   # Image → ESL bitmap converter (all models)
│   └── requirements.txt     # bleak, Pillow, click, pyserial (optional)
├── tools/
│   └── esl_image.py         # Standalone image → raw ESL buffer converter
├── docs/
│   ├── FLIPPER_INSTALL.md   # Flipper Zero installation guide (firmware + FAP + GPIO UART)
│   ├── HARDWARE.md          # How to identify compatible Hanshow tags + stock firmware FAQ
│   ├── FLASHING.md          # Step-by-step UART flash guide for ATC firmware
│   ├── PROTOCOL.md          # Full BLE GATT protocol documentation
│   └── WIRING.md            # UART wiring (USB-Serial adapter or Flipper GPIO)
└── README.md                # (this file)
```

---

## BLE Protocol Summary

All communication uses a single **BLE GATT characteristic write**:

| Service UUID | `13187B10-EBA9-A3BA-044E-83D3217D9A38` |
|---|---|
| **Char UUID** | `4B646063-6264-F3A7-8941-E65356EA82FE` |

| Command | Bytes | Action |
|---------|-------|--------|
| Clear | `0x00 0xFF` | Clear buffer to white |
| Set Position | `0x02 [hi] [lo]` | Seek to buffer offset |
| Write | `0x03 [data...]` | Write data bytes (≤239 bytes) |
| Display | `0x01` | Flush buffer → refresh display |

Full protocol documentation: [docs/PROTOCOL.md](docs/PROTOCOL.md)

---

## Image Format

- **Buffer size**: `width × ceil(height/8)` bytes (4000 bytes for 250×122)
- **Layout**: rotated 90° CW — each column is 16 consecutive bytes
- **Pixel**: `byte = (width−1−x) × col_bytes + (y>>3)`, `bit = 0x80 >> (y&7)`
- **Encoding**: black = 0, white = 1

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Tag not found in scan | Tag needs ATC firmware — see [FLASHING.md](docs/FLASHING.md) |
| `Unlock Flash` step fails | Hold the UART adapter at a slight angle; use 3.3 V logic levels only |
| Flipper can't see tag | Requires Unleashed/Momentum firmware for BLE Central mode |
| Display doesn't update | Wait 4–5 seconds after the upload completes |
| Garbled image | Verify the correct model is selected — pixel dimensions differ per model |
| `bleak` connection fails on Windows | Ensure Bluetooth adapter driver is up to date; try running as Administrator |

---

## Contributing

Pull requests welcome!  Areas that could use help:

- [ ] Flipper Zero OTA firmware update support (cmd `0x04` TIFF-G4 decode)
- [ ] Red-channel support for BWR models (second buffer plane)
- [ ] Auto-detect display model from BLE advertisement data
- [ ] Pre-built `.fap` binary releases via GitHub Actions

---

## License

MIT — see [LICENSE](LICENSE).

---

## Credits

- **[ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)** by Aaron Christophel (atc1441) — the open-source firmware that makes all of this possible.
- **[Flipper Zero](https://flipperzero.one/)** and the **[Unleashed Firmware](https://github.com/DarkFlippers/unleashed-firmware)** community.
- Hanshow Technology for manufacturing quality hardware worth repurposing.
