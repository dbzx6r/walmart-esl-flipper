# SES-imagotag / Vusion HRD-Series ESL Guide

This guide covers repurposing **SES-imagotag Vusion HRD-series** e-ink price tags
(e.g., the **HRD3-0210-A**) using the Python companion script.

These tags implement the **Bluetooth SIG Electronic Shelf Label Service v1.0**
(published March 2023), a fully documented open standard.

---

## What Makes These Tags Different

| Feature | Hanshow Stellar | SES-imagotag HRD3-0210-A |
|---------|----------------|--------------------------|
| SoC | Telink TLSR8359 | **Qualcomm QCC710** (BLE 5.3) |
| Protocol | ATC custom firmware (GATT) | BT SIG ESL Service 0x184D |
| Custom firmware required? | **Yes** | **No** — standard BLE spec |
| Flipper Zero FAP support | Full | Scan only (image upload needs PC) |
| Image upload | PC or Flipper | PC/Linux (OTP via L2CAP CoC) |

---

## Chip Identification

The HRD3-0210-A contains:

```
Qualcomm QCC710 002 | BTRTx008a
```

The **QCC710** is Qualcomm's dedicated BLE 5.3 chip for Electronic Shelf Labels.
Qualcomm co-authored the BT SIG ESL specification alongside SES-imagotag engineers —
which is why these tags implement the standard directly, with no proprietary firmware.

---

## BT SIG ESL Protocol Overview

### Service UUID

```
0x184D  — Electronic Shelf Label Service
```

Tags in **Unassociated** state advertise this UUID in their advertisement packets.

### GATT Characteristics

| Characteristic | UUID | Properties | Description |
|----------------|------|-----------|-------------|
| ESL Address | `0x2BF6` | Write (encrypted) | 15-bit address: Group_ID + ESL_ID |
| AP Sync Key Material | `0x2BF7` | Write (encrypted) | 24-byte session key |
| ESL Response Key Material | `0x2BF8` | Write (encrypted) | 24-byte response key |
| ESL Current Absolute Time | `0x2BF9` | Write (encrypted) | 32-bit ms timestamp |
| ESL Display Information | `0x2BFA` | Read (encrypted) | Display width/height/type |
| ESL Image Information | `0x2BFB` | Read (encrypted) | Max image slot index |
| ESL Control Point | `0x2BFE` | Write+Notify (encrypted) | TLV commands |

### State Machine

```
Unassociated  ──(bond + provision 4 chars)──►  Unsynchronized
Unsynchronized ──(connect)──►  Updating (image upload + commands)
Updating ──(update complete)──►  Unsynchronized
```

**Tags from eBay** return to **Unassociated** state ~60 minutes after losing contact
with the store AP — making them immediately ready to provision.

---

## Prerequisites

1. Python 3.8+  
2. A Bluetooth LE adapter (USB Bluetooth 5.0 dongle recommended)
3. Install dependencies:

```bash
cd companion
pip install -r requirements.txt
```

---

## Quick Start

### Step 1 — Scan for tags

```bash
python esl_companion.py vusion scan
```

Output:
```
Scanning for BT SIG ESL tags (service UUID 0x184D) for 12s...

Found 1 BT SIG ESL device(s):

  Address               Name                      RSSI
  ──────────────────────────────────────────────────────
  AA:BB:CC:DD:EE:FF     Vusion HRD3               -65 dBm
```

### Step 2 — Provision the tag

```bash
python esl_companion.py vusion provision AA:BB:CC:DD:EE:FF
```

Output:
```
  Connecting to AA:BB:CC:DD:EE:FF...
  Connected. Initiating BLE bonding...
  Bonded successfully.
  Writing provisioning characteristics...
    ESL Address       → group=0, id=0  [0000]
    AP Sync Key       → a3f1...
    Response Key      → 2b7c...
    Absolute Time     → 12345 ms  [39300000]
  Provisioning complete. Tag is now in Unsynchronized state.

  Tag info: ESL(group=0, id=0, images=0..3, displays=[Display[0]: 212×104px (Black/White)])
```

### Step 3 — Display a pre-stored image

```bash
python esl_companion.py vusion display AA:BB:CC:DD:EE:FF --image-index 0
```

Cycle through factory images:
```bash
python esl_companion.py vusion display AA:BB:CC:DD:EE:FF --image-index 1
python esl_companion.py vusion display AA:BB:CC:DD:EE:FF --image-index 2
```

### Step 4 — Upload a custom image (Linux only)

```bash
python esl_companion.py vusion upload-image AA:BB:CC:DD:EE:FF my_price_tag.png
```

> ⚠️ Image upload requires **Linux** with BlueZ.
> On Windows/macOS, use factory images with `vusion display`.

---

## All Vusion Commands

```bash
# Scan for nearby tags
python esl_companion.py vusion scan [--timeout 12]

# Provision a fresh (Unassociated) tag
python esl_companion.py vusion provision <ADDRESS> [--group 0] [--id 0]

# Read display info and image slot count
python esl_companion.py vusion info <ADDRESS> [--id 0]

# Display a pre-stored image
python esl_companion.py vusion display <ADDRESS> [--image-index 0] [--display-index 0] [--id 0]

# Ping the tag (check Basic State)
python esl_companion.py vusion ping <ADDRESS> [--id 0]

# Factory reset (returns to Unassociated state)
python esl_companion.py vusion reset <ADDRESS> [--confirm]

# Upload a custom image (Linux/BlueZ only)
python esl_companion.py vusion upload-image <ADDRESS> <image.png> [--image-index 0] [--color-mode bw]
```

---

## Provisioning Details

The provisioning step writes 4 mandatory characteristics that move the tag from
**Unassociated** → **Unsynchronized** state:

| Characteristic | Value written | Purpose |
|----------------|--------------|---------|
| ESL Address | `[group_id (7-bit) | esl_id (8-bit)]` | Assign a logical address |
| AP Sync Key Material | 24 random bytes | Session key for synchronized state |
| ESL Response Key Material | 24 random bytes | Authentication key for ESL responses |
| ESL Current Absolute Time | uint32 ms | Start the tag's internal clock |

The keys are only used in the **Synchronized** state (LE Periodic Advertising with
Responses, which requires a commercial AP infrastructure). For standalone use
(show images, cycle through content), any valid random bytes work.

**All writes require a bonded (encrypted) BLE connection.**  The tag uses Just Works
pairing — no PIN is required.

---

## Image Upload (OTP)

The BT SIG ESL spec uses the **Object Transfer Profile (OTP)** to upload images.
OTP sends large data over an **LE L2CAP Credit-Based Connection (CoC)** — a separate
BLE channel outside of GATT.

| Platform | Image Upload Support |
|----------|---------------------|
| Linux + BlueZ | ✅ Supported via `vusion upload-image` |
| Windows | ❌ Not supported (no L2CAP CoC in WinRT BLE API) |
| macOS | ❌ Not supported (CoreBluetooth does not expose L2CAP CoC) |
| Flipper Zero | ❌ Not supported (BLE Central OTP not implemented) |

### Image Format

The format is vendor-defined via the `Display_Type` field in ESL Display Information:

| Display_Type | Encoding | Bits/Pixel |
|-------------|---------|-----------|
| `0x01` (BW) | 1bpp, row-major, MSB first. 0=black, 1=white | 1 |
| `0x02` (BWR) | 2bpp, row-major, MSB first. 00=black, 01=white, 10=red | 2 |
| `0x03` (BWY) | 2bpp, same as BWR but 10=yellow | 2 |

Image size in bytes: `ceil(width × height × bits_per_pixel / 8)`

For a **212×104 BW** display (typical HRD3-0210-A): `ceil(212 × 104 / 8)` = **2756 bytes**

---

## Troubleshooting

### "No BT SIG ESL tags found"

- The tag broadcasts on a short interval — run a longer scan: `--timeout 20`
- Tags that were recently associated need ~60 min to return to Unassociated state
- Try inserting a fresh battery; voltage drops can prevent advertising
- Check with nRF Connect app: look for devices with service UUID `184D`

### "Bonding failed"

- Remove any existing pairing for the tag in your OS Bluetooth settings first
- On Linux: `bluetoothctl remove AA:BB:CC:DD:EE:FF` then retry
- On Windows: Devices & Printers → remove the device → retry

### "Write failed" after provisioning

- The tag may have entered an unexpected state; try a factory reset:
  ```bash
  python esl_companion.py vusion reset AA:BB:CC:DD:EE:FF
  ```
- Wait 10 seconds, then reprovision

### "NotImplementedError: OTP image upload via L2CAP CoC is only supported on Linux"

- Use `vusion display` to cycle through factory images on Windows/macOS
- Or run on Linux with BlueZ for custom image upload

---

## Using Flipper Zero with Vusion Tags

The Flipper Zero FAP (`flipper-app/`) can **scan** for HRD3-series tags and display
them in the scan list. Full image upload is not supported via the Flipper.

To use the Flipper with Vusion tags:
1. Use `vusion provision` on your PC first to provision the tag
2. Use `vusion display` from your PC to load an image
3. The Flipper can then display the same image via `Refresh Display` (future feature)

---

## Protocol Reference

Full specification: [Bluetooth SIG ESLS v1.0 (2023-03-28)](https://www.bluetooth.com/specifications/specs/electronic-shelf-label-profile-1-0/)

Key sections:
- §2.7.3 — State machine (Unassociated, Configuring, Synchronized, Updating, Unsynchronized)
- §3.1–3.9 — All GATT characteristics
- §3.9.2 — Control Point command TLV format
- OTP: Bluetooth SIG Object Transfer Profile v1.0

---

## eBay Tag Identification

When buying SES-imagotag tags on eBay:

**Product codes to look for:**
- `HRD3-XXXX` — 3rd-generation Vusion HD series (QCC710 chip) ✅
- `HD150E`, `HD290B`, `HD29N` — older series (may use different protocol) ⚠️
- Tags with `Vusion` branding — generally HRD3 series ✅

**Physical identification:**
- Back label: `SES-imagotag` or `Vusion` logo
- Model number: `HRD3-XXXX-X`
- Opens with a small plastic clip; QCC710 chip visible on PCB

**Battery:** Most HRD3 tags use a **CR2032** coin cell (some larger models use CR2450).
