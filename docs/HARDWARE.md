# Hardware Identification Guide

This guide helps you identify which Hanshow Stellar e-ink price tags are compatible
with the ATC_TLSR_Paper custom firmware and this tool.

---

## Compatible Tags

All compatible tags use the **Telink TLSR8359** SoC (System on Chip), which provides:
- ARM Cortex-M0 processor
- Built-in BLE 5.0 radio
- SPI interface for the e-ink display

### How to Identify Compatible Tags

Look for the model label on the **back** of the tag. Compatible models contain the letter `N`
in their model name, which indicates an NFC chip (and also indicates the TLSR8359 SoC).

**Model naming convention:**
```
Stellar - [SIZE][COLORS][FEATURES] [HW_REV]

SIZE:     M = 2.13 inch,  S = 1.54 inch,  L = larger
COLORS:   F = 2-color (BW only),  3 = 3-color (BWR),  Y = 3-color (BWY)
FEATURES: N = NFC chip (required!), A = No reed switch, @ = LED light
```

---

## Confirmed Compatible Models

### Stellar-MN@ E31H — 2.13" Black/White
- **Display:** 250×122 pixels, black and white only
- **Size:** 2.13 inches diagonal
- **Back label:** `Stellar-MN@ E31H`
- **SoC:** Telink TLSR8359

### Stellar-M3N@ E31HA — 2.13" Black/White/Red
- **Display:** 250×122 pixels, black, white, and red
- **Size:** 2.13 inches diagonal
- **Back label:** `Stellar-M3N@ E31HA`
- **SoC:** Telink TLSR8359

### Stellar-MFN@ E31A — 2.13" Black/White (alt)
- **Display:** 212×104 pixels, black and white only
- **Size:** 2.13 inches diagonal
- **Back label:** `Stellar-MFN@ E31A`
- **SoC:** Telink TLSR8359

### Stellar-S3TN@ E31HA — 1.54" Black/White/Red
- **Display:** 200×200 pixels, black, white, and red
- **Size:** 1.54 inches diagonal
- **Back label:** `Stellar-S3TN@ E31HA`
- **SoC:** Telink TLSR8359

---

## ❓ Do I Need to Flash Custom Firmware on the Tags?

**Yes — the custom firmware (ATC_TLSR_Paper) is required before the tags will respond to any open commands.**

Here's why:

### The stock Hanshow firmware is completely proprietary

The tags from Walmart run Hanshow's own firmware, which uses an **encrypted, undocumented BLE protocol** designed to communicate only with Hanshow's commercial base stations (the white boxes mounted on store shelves).

- No public specification exists for this protocol.
- Without a Hanshow base station, the tags cannot be commanded in any way.
- The tags do advertise over BLE in stock mode, but all data is encrypted and write commands are rejected.

### ATC_TLSR_Paper replaces the entire firmware

The one-time UART flash (see [FLASHING.md](FLASHING.md)) replaces the stock firmware with an open-source implementation that exposes a simple, documented BLE GATT interface.  After this flash:

- No more Hanshow infrastructure required
- Any BLE Central device (Flipper Zero, phone, PC) can control the tag
- The process takes ~2 minutes per tag

### Tags you buy on eBay may already be flashed

Many eBay lots come from liquidated store inventory where someone has already installed ATC firmware.  If the tag shows a screen with the text `ESL_XXXXXX` or a battery/temperature readout, ATC firmware is already installed and you can skip flashing.

To check: use a BLE scanner app (nRF Connect, LightBlue) and scan for devices.  If you see one named `ESL_` followed by 6 hex characters, the tag is ready to use.

### Alternatives to the Flipper app

Once tags have ATC firmware, you have multiple options — **none of them require a Flipper Zero**:

| Method | Requirements |
|--------|-------------|
| Python companion (`esl_companion.py`) | PC/Mac/Linux + Bluetooth adapter |
| ATC WebBluetooth uploader | Chrome/Edge browser + Bluetooth |
| This Flipper Zero FAP | Flipper with Unleashed/Momentum firmware |

The WebBluetooth uploader (browser-based, no install) is the simplest:  
**https://atc1441.github.io/ATC_TLSR_Paper_Image_Upload.html**

---

## ❌ Incompatible Models

Models **without** the `N` designator use a different SoC (not TLSR8359) and are **not compatible**:

| Model | Reason Incompatible |
|-------|---------------------|
| Stellar-M@ | No NFC/TLSR chip |
| Stellar-M3@ | No NFC/TLSR chip |
| Solum ZBS243-based tags | Different SoC entirely (ZBS243) |
| SES-imagotag VUSION tags | Proprietary locked firmware |

---

## What to Look for on eBay

When searching eBay for compatible tags, use these search terms:
- `"Hanshow Stellar" ESL`
- `"Hanshow" price tag e-ink`
- `"Stellar-MN"` or `"Stellar-M3N"`

**Tips:**
- Lots of 10–50 tags are common and cost $1–5 per tag
- Look for clear photos showing the back label
- Listings from liquidated store inventory are most reliable
- Ask the seller for the model number if not shown

---

## Physical Appearance

### Front
- E-ink display showing a price
- Usually a small Walmart or store logo in the corner
- May have a colored border/strip (red/yellow = BWR/BWY variant)

### Back
- White plastic housing
- Model label sticker (e.g., `Stellar-MN@ E31H`)
- Sometimes a barcode
- Battery slot (usually CR2032 or CR2450)

### LED Indicator
Models with `@` in the name have a small LED (usually red) that blinks when the BLE
connection is active.

---

## Battery Information

| Display Size | Battery Type | Approximate Life |
|-------------|-------------|-----------------|
| 1.54"       | CR2032      | 2–3 years normal use |
| 2.13"       | CR2450      | 3–5 years normal use |

With custom firmware and frequent BLE updates, battery life will be shorter.

---

## UART Flashing Points (for firmware flashing)

Refer to [WIRING.md](WIRING.md) for pinout diagrams.

The UART test points are located on the PCB inside the tag. You need to open the
housing carefully (usually a plastic clip on the back) to access them.

Key pads on Stellar-MN@ / M3N@:
- **VCC:** 3.3V power (connect to your USB-Serial adapter's 3.3V)
- **GND:** Ground
- **TX:** Tag transmit → connect to USB-Serial RX
- **RX:** Tag receive → connect to USB-Serial TX
- **RST:** Reset pin (optional, for reliable flashing)
