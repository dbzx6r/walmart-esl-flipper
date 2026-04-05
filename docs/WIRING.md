# UART Wiring Reference

See the full flashing guide at [FLASHING.md](FLASHING.md).  
For Flipper Zero installation instructions, see [FLIPPER_INSTALL.md](FLIPPER_INSTALL.md).

## Option A — USB-Serial Adapter (CH340 / CP2102 / FTDI)

### Quick Reference Wiring Table

```
Tag PCB Pin   →  USB-Serial Adapter
──────────────────────────────────────
VCC (3.3V)   →  3.3V  (NOT 5V!)
GND          →  GND
TX           →  RX
RX           →  TX
RST          →  GND (only during flash unlock)
```

## Option B — Flipper Zero as USB-UART Bridge (no extra hardware needed)

You can use your Flipper Zero's GPIO header instead of a separate USB-Serial adapter.

### Wiring: Flipper GPIO → Tag PCB

```
Flipper GPIO Header          Tag PCB Pad
─────────────────────────────────────────
Pin 1  (3.3V out)   ──────→  VCC
Pin 8  (GND)        ──────→  GND
Pin 13 (TX / C3)    ──────→  RX   ← tag receive
Pin 14 (RX / C4)    ──────→  TX   ← tag transmit
RST pad on tag      ──────→  GND  (only during flash unlock step)
```

> Flipper GPIO is 3.3V — safe for the TLSR8359.  Never use 5V.

**To enable the UART bridge on the Flipper:**  
`GPIO → USB-UART Bridge → baud: 115200, pins: TX/RX (13/14)`

The Flipper will then appear as a USB CDC serial port on your PC, which the
WebSerial flasher can connect to directly.

---

## Official Wiring Photo

Refer to the official photo for exact pad locations:
https://github.com/atc1441/ATC_TLSR_Paper/blob/main/USB_UART_Flashing_connection.jpg

## Hanshow Stellar-MN@ Pinout (from community documentation)

```
PCB top view (battery removed, display cable at top):

           ┌─ Display FPC connector ─┐
           │                         │
     ┌─────┴─────────────────────────┴─────┐
     │                 [TLSR8359]           │
     │                                     │
     │  ●    ●    ●    ●    ●              │
     │  3V3  GND  TX   RX   RST            │
     │                                     │
     │            [Battery holder]         │
     └─────────────────────────────────────┘
```

Pads are located on the bottom edge of the PCB near the battery holder.
They are small (1–1.5 mm) solder pads — use fine magnet wire for reliable connections.
