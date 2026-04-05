# UART Wiring Guide for Firmware Flashing

This guide explains how to wire a USB-Serial adapter to a Hanshow Stellar tag for
flashing the ATC_TLSR_Paper custom firmware.

---

## Required Equipment

| Item | Notes |
|------|-------|
| USB-to-Serial adapter (3.3V TTL) | CH340, CP2102, or FTDI232 — must support 3.3V |
| Fine-tip soldering iron | For connecting to small PCB pads |
| Thin wire / magnet wire | 28–30 AWG works well |
| Isopropyl alcohol | For cleaning flux |
| Computer with Chrome/Edge browser | For the WebSerial flasher tool |

⚠️ **IMPORTANT:** Use a **3.3V** adapter only. 5V will damage the TLSR8359 chip.

---

## Wiring Diagram

```
Hanshow Stellar Tag PCB          USB-Serial Adapter
─────────────────────────        ─────────────────
VCC (3.3V)  ──────────────────── 3.3V
GND         ──────────────────── GND
TX          ──────────────────── RX
RX          ──────────────────── TX
```

> Note: TX of tag → RX of adapter, RX of tag → TX of adapter (crossed).

---

## Pinout: Stellar-MN@ E31H / Stellar-M3N@ E31HA

```
Top view of PCB (battery removed):

    ┌──────────────────────────────────────────┐
    │  [E-ink connector]    [TLSR8359 SoC]     │
    │                                           │
    │  ○ VCC  ○ GND  ○ TX  ○ RX  ○ RST        │
    │    │      │      │     │      │            │
    │    3.3V   GND   TX   RX    Reset           │
    └──────────────────────────────────────────┘

Pads are typically labeled on the PCB silkscreen or described in the image:
https://github.com/atc1441/ATC_TLSR_Paper/blob/main/USB_UART_Flashing_connection.jpg
```

For the exact pad locations, refer to Aaron Christophel's wiring photo:
**https://github.com/atc1441/ATC_TLSR_Paper/blob/main/USB_UART_Flashing_connection.jpg**

---

## Opening the Tag

1. **Remove the battery** before opening.
2. Look for a small notch or clip on the back of the housing.
3. Gently insert a plastic spudger or guitar pick into the seam.
4. Carefully pry around the perimeter — the housing usually snaps off.
5. The PCB is exposed; handle by the edges to avoid static discharge.

---

## Step-by-Step Flashing

### Step 1: Wire up the adapter
Connect according to the wiring diagram above.
Do **not** connect power yet.

### Step 2: Open the WebSerial Flasher
In **Chrome or Edge**, navigate to:
```
https://atc1441.github.io/ATC_TLSR_Paper_UART_Flasher.html
```

### Step 3: Connect to the adapter
1. Click **"Connect"** on the web page.
2. A dialog will show your USB-Serial adapter port — select it and click **Connect**.

### Step 4: Unlock the flash (first time only)
1. With the tag powered off (battery removed), hold **RST low** or bridge the RST pad to GND.
2. Connect power (3.3V).
3. On the flasher page, click **"Unlock Flash"** immediately after powering on.
4. Wait for confirmation.

> The TLSR8359 ships with flash protection enabled. This step removes it.
> Only needed once per tag.

### Step 5: Flash the firmware
1. Click **"Choose .bin file"** and select `ATC_Paper.bin` from the firmware folder.
2. Click **"Write Firmware"**.
3. Wait for the progress bar to complete (~30–60 seconds).
4. The flasher will show "Done!" when complete.

### Step 6: Verify
1. Disconnect the serial adapter.
2. Insert the battery.
3. The display should refresh and show the ATC firmware default screen
   (device name like `ESL_XXXXXX`, battery level, temperature).
4. Use a BLE scanner app (nRF Connect, LightBlue) to confirm the device is advertising
   as `ESL_XXXXXX`.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Flasher won't connect | Try a different USB port; reinstall CH340/CP2102 drivers |
| "Unlock failed" | Make sure RST is held low exactly when powering on |
| Gibberish on display after flash | Flash may be incomplete; retry |
| Device not advertising after flash | Check battery polarity; try re-flashing |
| Tag gets hot | Disconnect immediately — wiring error (check VCC/GND polarity) |

---

## After Flashing

Once the ATC firmware is installed, you never need to wire-flash again.
All future updates (new images, firmware OTA) are done wirelessly via BLE.

To revert to stock firmware, you would need the original Hanshow firmware binary
(not publicly available) and the same UART flashing process.

---

## Resources

- **ATC_TLSR_Paper GitHub:** https://github.com/atc1441/ATC_TLSR_Paper
- **WebSerial Flasher:** https://atc1441.github.io/ATC_TLSR_Paper_UART_Flasher.html
- **WebBluetooth Image Uploader:** https://atc1441.github.io/ATC_TLSR_Paper_Image_Upload.html
- **Wiring Photo:** https://github.com/atc1441/ATC_TLSR_Paper/blob/main/USB_UART_Flashing_connection.jpg
