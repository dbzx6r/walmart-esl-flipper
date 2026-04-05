# ESL BLE Protocol Documentation

This document describes the BLE GATT protocol used by Hanshow Stellar e-ink price tags
after flashing the [ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper) custom firmware.

---

## BLE Advertisement

After flashing, the tag advertises as:
- **Device Name:** `ESL_XXXXXX` (where `XXXXXX` is the last 3 bytes of the MAC address in uppercase hex)
- **Example:** A tag with MAC `A4:C1:38:01:23:45` advertises as `ESL_012345`

---

## GATT Services and Characteristics

### EPD Image Service (Primary)

| Field              | Value                                      |
|--------------------|--------------------------------------------|
| Service UUID       | `13187B10-EBA9-A3BA-044E-83D3217D9A38`     |
| Characteristic UUID| `4B646063-6264-F3A7-8941-E65356EA82FE`     |
| Properties         | Write (no response / write with response)  |

This is the main service for uploading images and controlling the display.

### RxTx Control Service

| Field              | Value  |
|--------------------|--------|
| Service UUID       | `0x1F10` |
| Characteristic UUID| `0x1F1F` |
| Properties         | Write + Notify |

Used for device control commands (time, intervals, EPD model).

### OTA Firmware Update Service

| Field              | Value  |
|--------------------|--------|
| Service UUID       | `0x221F` |
| Characteristic UUID| `0x331F` |
| Properties         | Write + Notify |

Used for over-the-air firmware updates (advanced use only).

---

## EPD Image Service Commands

All commands are written to the EPD characteristic (`4B646063-...`).
The first byte of each write is the command byte.

### `0x00` — Clear Buffer

```
Payload: [0x00] [fill_byte]
```

Fills the entire image buffer with `fill_byte`.
- `0xFF` = all white
- `0x00` = all black

**Example:** `[0x00, 0xFF]` → clear to white

---

### `0x02` — Set Write Position

```
Payload: [0x02] [position_high_byte] [position_low_byte]
```

Sets the current buffer write pointer to a 16-bit big-endian position.

**Example:** `[0x02, 0x00, 0x00]` → set position to 0 (beginning of buffer)

---

### `0x03` — Write Data to Buffer

```
Payload: [0x03] [data_byte_0] [data_byte_1] ... [data_byte_N]
```

Writes N bytes to the image buffer starting at the current write position.
After the write, position advances by N bytes automatically.

The maximum payload size is limited by BLE MTU (typically 240 bytes per write,
so max 239 data bytes per command).

**Example:** `[0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00]` → writes 5 bytes at current position

---

### `0x01` — Display Buffer

```
Payload: [0x01]
```

Triggers a full e-ink display refresh using the current buffer contents.
This causes the display to visibly update (takes 2–4 seconds for e-ink refresh).

---

### `0x04` — Decode and Display TIFF

```
Payload: [0x04]
```

Interprets the data currently in the buffer as a TIFF-G4 compressed image,
decodes it, and updates the display. The byte_pos value should equal the
length of the TIFF data written to the buffer.

---

## RxTx Control Commands

Commands written to the RxTx characteristic (`0x1F1F`):

| Byte 0 | Byte 1 | Description                                  |
|--------|--------|----------------------------------------------|
| `0xE0` | `model`| Force set EPD model (0–6, see table below)   |
| `0xDD` | `t[3]`…`t[0]` | Set RTC time (4 bytes, big-endian Unix seconds) |
| `0xFE` | `interval` | Set BLE advertising interval (×10 seconds) |
| `0xDF` | —      | Save current settings to flash               |
| `0xDE` | —      | Reset settings to defaults and save          |
| `0xB1` | `byte` | Fill entire buffer with `byte` and display   |

**EPD Model Numbers:**

| Model # | Display Type |
|---------|-------------|
| 1       | BW213 (250×122 Black/White 2.13") |
| 2       | BWR213 (250×122 Black/White/Red 2.13") |
| 3       | BWR154 (200×200 Black/White/Red 1.54") |
| 4       | BW213ICE (212×104 Black/White 2.13" alt) |
| 5       | BWR350 (Black/White/Red 3.5") |
| 6       | BWY350 (Black/White/Yellow 3.5") |

---

## Image Buffer Format

### Buffer Layout

The image buffer is stored **rotated 90° clockwise** relative to the physical display orientation.

For the standard 250×122 BW213 display:
- **Buffer size:** 4000 bytes (250 columns × 16 bytes/column)
- **Physical display:** 250 pixels wide × 122 pixels tall (landscape)
- **Memory layout:** 250 "columns" of 16 bytes each

### Pixel Coordinate Mapping

Given a pixel at display coordinates `(x, y)` where:
- `x` = 0 (left) to 249 (right)
- `y` = 0 (top) to 121 (bottom)

The pixel is stored in the buffer at:
```
byte_index = (y >> 3) + (249 - x) * 16
bit_mask   = 0x80 >> (y & 7)
```

**Encoding:**
- `bit = 0` → **black** pixel
- `bit = 1` → **white** pixel (inverted from typical convention)

### Initializing the Buffer

Before writing a new image, always initialize the buffer to white:
```
Write: [0x02, 0x00, 0x00]    <- set position to 0
Write: [0x00, 0xFF]           <- fill with 0xFF (white)
```

Or send `[0x00, 0xFF]` first (which doesn't use position).

---

## Complete Image Upload Sequence

```
Step 1: Connect to BLE device "ESL_XXXXXX"
Step 2: Discover service 13187B10-EBA9-A3BA-044E-83D3217D9A38
Step 3: Get write handle for characteristic 4B646063-6264-F3A7-8941-E65356EA82FE

Step 4: Send [0x00, 0xFF]              <- clear to white
Step 5: Send [0x02, 0x00, 0x00]        <- set write position to 0

Step 6: For each 239-byte chunk of image data:
           Send [0x03, chunk_byte_0, chunk_byte_1, ..., chunk_byte_N]

Step 7: Send [0x01]                    <- trigger display refresh
```

Total data for 250×122 display: 4000 bytes → ~17 BLE write packets of 239 bytes each.

---

## Display Refresh Timing

E-ink displays require time to refresh physically:
- **Black/White:** ~2 seconds
- **Black/White/Red or Yellow:** ~3–4 seconds

Do NOT send a new image while the display is refreshing. Wait at least 4 seconds after
the final `0x01` command before starting a new upload.

---

## BLE Connection Parameters

The ATC firmware requests these connection parameters after connecting:
- **Connection interval:** 200–202 ms (standard)
- **During image upload:** Speeds up to 7.5 ms interval automatically

The firmware uses **BLE 1M PHY** (standard BLE 4.x mode, compatible with all BLE 4.0+ hardware).

---

## Notes for Flipper Zero Users

The Flipper Zero's STM32WB55 chip supports BLE 5.0. For BLE Central (GATT client) mode:
- **Community firmware** (Unleashed, Momentum): full BLE GATT client support
- **Official firmware**: use the companion Python script via USB Serial instead

See [README.md](../README.md) for the USB Serial companion workflow.
