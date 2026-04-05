# ESP32 BLE Bridge Firmware

This folder contains the Arduino sketch that turns any ESP32 module into the BLE bridge needed by the **ESL Tool** Flipper Zero app.

---

## Compatible Hardware

| Module | BLE | Notes |
|--------|-----|-------|
| **Seeed XIAO ESP32C3** ($5) | BLE 5.0 | Best budget choice — tiny, USB-C, 3.3V native |
| **ESP32-WROOM-32D** ($10) | BLE 4.2 | Most common Amazon devkit |
| **Mayhem v2 ESP32-CAM** ($95) | BLE 4.2 | Plug-in for Flipper, no wiring needed |
| ESP32-S3, ESP32-C6 | BLE 5.0 | Also work |
| ❌ ESP8266 | WiFi only | Does NOT work — no BLE hardware |
| ❌ ESP32-S2 | WiFi only | Does NOT work — no BLE hardware |

---

## Wiring (XIAO ESP32C3 and WROOM-32D)

> Mayhem v2 users: no wiring required — it plugs directly into the Flipper GPIO header.

```
Flipper GPIO pin 13 (C1 / TX) ──→  ESP32 RX
Flipper GPIO pin 14 (C0 / RX) ──→  ESP32 TX
Flipper GPIO pin 18 (GND)     ──→  ESP32 GND
Flipper GPIO pin  9 (3.3V)    ──→  ESP32 3.3V  ⚠ NOT 5V!
```

| Module | ESP32 RX pin | ESP32 TX pin |
|--------|-------------|-------------|
| XIAO ESP32C3 | GPIO 20 | GPIO 21 |
| WROOM-32D / DevKit | GPIO 16 | GPIO 17 |
| Mayhem v2 | Auto | Auto |

The sketch defaults to GPIO 20/21 (XIAO). To change, edit these lines near the top of `esp32_ble_bridge.ino`:

```cpp
#define RX_PIN   20   // ← change to 16 for WROOM-32D
#define TX_PIN   21   // ← change to 17 for WROOM-32D
```

---

## Arduino IDE Setup

### 1. Install ESP32 board support

1. Open **Arduino IDE → Preferences**
2. Add to "Additional Boards Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Tools → Board → Boards Manager**, search "esp32", install **"esp32 by Espressif Systems"** (v2.x or v3.x)

### 2. Install required libraries

Open **Sketch → Include Library → Manage Libraries** and install:

| Library | Author | Notes |
|---------|--------|-------|
| **NimBLE-Arduino** | h2zero | BLE Central + bonding (required) |

### 3. Select board and port

| Module | Board selection |
|--------|----------------|
| XIAO ESP32C3 | `XIAO_ESP32C3` |
| WROOM-32D / DevKit | `ESP32 Dev Module` |
| Mayhem v2 | `AI Thinker ESP32-CAM` |

Set **Tools → Upload Speed → 115200**, **Tools → Partition Scheme → Default 4MB with spiffs**.

### 4. Flash the sketch

1. Open `esp32_ble_bridge.ino` in Arduino IDE
2. Select the correct board and serial port
3. Click **Upload** (⬆)
4. Open the Serial Monitor at 115200 baud — you should see:
   ```
   [ESL Bridge] Ready. Waiting for Flipper commands.
   ```

---

## Testing

With the ESP32 connected to your computer via USB (not the Flipper), open a terminal and connect to the ESP32's serial port at 115200 baud. Type commands manually:

```
SCAN
```
Expected output (example):
```
DEVICE ESL_A1B2C3 AA:BB:CC:DD:EE:FF -72 ATC
DEVICE ? 11:22:33:44:55:66 -68 VUSION
DONE
```

```
ATC_PRICE AA:BB:CC:DD:EE:FF $4.99
```
Expected:
```
PROGRESS 0
PROGRESS 10
...
PROGRESS 100
OK Price set
```

---

## Protocol Reference

See `docs/PROTOCOL.md` in the project root for the full UART serial protocol specification.
