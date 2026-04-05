#!/usr/bin/env python3
"""
ESL Companion — BLE companion script for Hanshow Stellar e-ink price tags.

Works with tags running ATC_TLSR_Paper firmware (https://github.com/atc1441/ATC_TLSR_Paper).
Can be used standalone (direct BLE) or as a backend for the Flipper Zero FAP via USB Serial.

Usage:
    python esl_companion.py scan
    python esl_companion.py price "AA:BB:CC:DD:EE:FF" "$12.99" --label "Great Value"
    python esl_companion.py image "AA:BB:CC:DD:EE:FF" photo.jpg
    python esl_companion.py clear "AA:BB:CC:DD:EE:FF"
    python esl_companion.py serial --port COM3   # USB Serial mode (listen for Flipper)

Requirements:
    pip install bleak Pillow click
"""

from __future__ import annotations

import asyncio
import sys
import time
from typing import Optional

import click

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
    from bleak.exc import BleakError
except ImportError:
    click.echo("Error: 'bleak' package not installed. Run: pip install bleak", err=True)
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    click.echo("Error: 'Pillow' package not installed. Run: pip install Pillow", err=True)
    sys.exit(1)

from image_converter import DisplayModel, DISPLAY_SIZE, image_to_esl, render_price_tag
import esl_vusion

# ── BLE GATT UUIDs (from ATC_TLSR_Paper firmware app_att.c) ──────────────────

EPD_SERVICE_UUID = "13187b10-eba9-a3ba-044e-83d3217d9a38"
EPD_CHAR_UUID    = "4b646063-6264-f3a7-8941-e65356ea82fe"

RXTX_SERVICE_UUID = "0000101f-0000-1000-8000-00805f9b34fb"  # short UUID 0x1F10
RXTX_CHAR_UUID    = "00001f1f-0000-1000-8000-00805f9b34fb"  # short UUID 0x1F1F

# EPD service command bytes
CMD_CLEAR    = 0x00
CMD_DISPLAY  = 0x01
CMD_SET_POS  = 0x02
CMD_WRITE    = 0x03
CMD_TIFF     = 0x04

# Maximum bytes per BLE write (MTU 247 − 3 ATT overhead − 1 command byte = 243 data bytes)
# Using 239 to be safe across different adapters
CHUNK_SIZE = 239

# Time to wait after display refresh command (e-ink needs time to update)
DISPLAY_REFRESH_WAIT_S = 4.0

# ESL device name prefix
ESL_NAME_PREFIX = "ESL_"

# ── Model detection ────────────────────────────────────────────────────────────

MODEL_MAP: dict[str, DisplayModel] = {
    "BW213":    DisplayModel.BW213,
    "BWR213":   DisplayModel.BWR213,
    "BWR154":   DisplayModel.BWR154,
    "BW213ICE": DisplayModel.BW213ICE,
    "BWR350":   DisplayModel.BWR350,
    "BWY350":   DisplayModel.BWY350,
}


# ── Low-level BLE operations ───────────────────────────────────────────────────

async def _write_epd(client: BleakClient, data: bytes) -> None:
    """Write bytes to the EPD characteristic (with response)."""
    await client.write_gatt_char(EPD_CHAR_UUID, data, response=True)


async def _clear_display(client: BleakClient, fill: int = 0xFF) -> None:
    """Clear the image buffer (0xFF = white, 0x00 = black)."""
    await _write_epd(client, bytes([CMD_CLEAR, fill]))


async def _set_position(client: BleakClient, pos: int) -> None:
    """Set the write position in the image buffer."""
    await _write_epd(client, bytes([CMD_SET_POS, (pos >> 8) & 0xFF, pos & 0xFF]))


async def _upload_buffer(client: BleakClient, buf: bytes, progress: bool = True) -> None:
    """
    Upload a raw image buffer to the tag.

    Sends the buffer in CHUNK_SIZE chunks, preceded by a position-set command.
    """
    await _clear_display(client, 0xFF)
    await _set_position(client, 0)

    total = len(buf)
    sent = 0
    chunk_count = 0

    while sent < total:
        chunk = buf[sent : sent + CHUNK_SIZE]
        await _write_epd(client, bytes([CMD_WRITE]) + chunk)
        sent += len(chunk)
        chunk_count += 1
        if progress:
            pct = int(sent / total * 100)
            click.echo(f"\r  Uploading... {pct}% ({sent}/{total} bytes)", nl=False)

    if progress:
        click.echo()  # newline after progress

    await _write_epd(client, bytes([CMD_DISPLAY]))
    if progress:
        click.echo(f"  Display refresh triggered. Waiting {DISPLAY_REFRESH_WAIT_S:.0f}s...")
    await asyncio.sleep(DISPLAY_REFRESH_WAIT_S)
    if progress:
        click.echo("  Done.")


async def _find_device(address: Optional[str]) -> BLEDevice:
    """
    Find an ESL BLE device.

    If address is given, scan for that specific device.
    If address is None, scan and return the first ESL_ device found.
    """
    click.echo("  Scanning for BLE devices...")
    timeout = 10.0

    if address:
        device = await BleakScanner.find_device_by_address(address, timeout=timeout)
        if device is None:
            raise click.ClickException(f"Device {address} not found within {timeout:.0f}s")
        return device

    # Scan for first ESL_ device
    found = await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name is not None and d.name.startswith(ESL_NAME_PREFIX),
        timeout=timeout,
    )
    if found is None:
        raise click.ClickException(
            f"No ESL_ device found within {timeout:.0f}s. "
            "Make sure the tag is powered on and within BLE range."
        )
    return found


# ── CLI interface ──────────────────────────────────────────────────────────────

@click.group()
def cli():
    """ESL Companion — control Hanshow Stellar and SES-imagotag/Vusion e-ink price tags over BLE."""


@cli.command()
@click.option("--timeout", default=10.0, help="Scan duration in seconds.", show_default=True)
def scan(timeout: float):
    """Scan for nearby ESL tags and list them."""

    async def _scan():
        click.echo(f"Scanning for {timeout:.0f}s...")
        devices = await BleakScanner.discover(timeout=timeout)
        esl_devices = [d for d in devices if d.name and d.name.startswith(ESL_NAME_PREFIX)]

        if not esl_devices:
            click.echo("No ESL devices found.")
            return

        click.echo(f"\nFound {len(esl_devices)} ESL device(s):\n")
        click.echo(f"  {'Address':<20}  {'Name':<16}  RSSI")
        click.echo("  " + "─" * 50)
        for d in esl_devices:
            rssi = getattr(d, "rssi", "?")
            click.echo(f"  {d.address:<20}  {d.name:<16}  {rssi} dBm")

    asyncio.run(_scan())


@cli.command()
@click.argument("address")
@click.argument("price_text")
@click.option("--label", default="", help="Small label text above the price.")
@click.option("--model", default="BW213", type=click.Choice(list(MODEL_MAP)), show_default=True,
              help="Display model.")
@click.option("--invert", is_flag=True, help="Invert black/white.")
def price(address: str, price_text: str, label: str, model: str, invert: bool):
    """Upload a price tag layout to a device.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    PRICE_TEXT is the price string, e.g. "$12.99"
    """
    display_model = MODEL_MAP[model]

    async def _upload():
        click.echo(f"Rendering price: {price_text!r}  label: {label!r}")
        buf = render_price_tag(price_text, label=label, model=display_model)
        click.echo(f"  Buffer size: {len(buf)} bytes")
        device = await _find_device(address)
        click.echo(f"  Connecting to {device.name} ({device.address})...")
        async with BleakClient(device) as client:
            click.echo("  Connected.")
            await _upload_buffer(client, buf)

    asyncio.run(_upload())


@cli.command()
@click.argument("address")
@click.argument("image_path", type=click.Path(exists=True))
@click.option("--model", default="BW213", type=click.Choice(list(MODEL_MAP)), show_default=True,
              help="Display model.")
@click.option("--no-dither", is_flag=True, help="Disable dithering.")
@click.option("--invert", is_flag=True, help="Invert black/white.")
def image(address: str, image_path: str, model: str, no_dither: bool, invert: bool):
    """Upload an image file to a device.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    IMAGE_PATH is the path to any image file (JPG, PNG, BMP, etc.)
    """
    display_model = MODEL_MAP[model]

    async def _upload():
        click.echo(f"Loading image: {image_path}")
        src_img = Image.open(image_path)
        buf = image_to_esl(src_img, model=display_model, dither=not no_dither, invert=invert)
        click.echo(f"  Buffer size: {len(buf)} bytes")
        device = await _find_device(address)
        click.echo(f"  Connecting to {device.name} ({device.address})...")
        async with BleakClient(device) as client:
            click.echo("  Connected.")
            await _upload_buffer(client, buf)

    asyncio.run(_upload())


@cli.command()
@click.argument("address")
@click.option("--fill", default="white", type=click.Choice(["white", "black"]),
              help="Fill color.", show_default=True)
def clear(address: str, fill: str):
    """Clear the display (fill with white or black).

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    """
    fill_byte = 0xFF if fill == "white" else 0x00

    async def _clear():
        device = await _find_device(address)
        click.echo(f"  Connecting to {device.name} ({device.address})...")
        async with BleakClient(device) as client:
            click.echo("  Connected. Clearing...")
            await _clear_display(client, fill_byte)
            await _write_epd(client, bytes([CMD_DISPLAY]))
            click.echo(f"  Display cleared to {fill}.")
            await asyncio.sleep(DISPLAY_REFRESH_WAIT_S)

    asyncio.run(_clear())


@cli.command()
@click.argument("address")
def info(address: str):
    """Read device info (battery level, temperature).

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    """
    BATTERY_CHAR_UUID = "00002a19-0000-1000-8000-00805f9b34fb"
    TEMP_CHAR_UUID    = "00002a1f-0000-1000-8000-00805f9b34fb"

    async def _info():
        device = await _find_device(address)
        click.echo(f"  Connecting to {device.name} ({device.address})...")
        async with BleakClient(device) as client:
            click.echo("  Connected.")
            try:
                batt = await client.read_gatt_char(BATTERY_CHAR_UUID)
                click.echo(f"  Battery level: {batt[0]}%")
            except Exception:
                click.echo("  Battery level: unavailable")
            try:
                temp_raw = await client.read_gatt_char(TEMP_CHAR_UUID)
                temp_c = (temp_raw[1] << 8 | temp_raw[0]) / 10.0
                click.echo(f"  Temperature:   {temp_c:.1f}°C")
            except Exception:
                click.echo("  Temperature: unavailable")

    asyncio.run(_info())


@cli.command()
@click.option("--port", required=True, help="Serial port (e.g. COM3 or /dev/ttyACM0).")
@click.option("--baud", default=115200, show_default=True, help="Baud rate.")
def serial(port: str, baud: int):
    """
    USB Serial bridge mode — listen for commands from the Flipper Zero FAP.

    The Flipper Zero app sends newline-delimited commands over USB CDC serial:

      ESL:SCAN
      ESL:PRICE:<MAC>:<model>:<price_text>:<label_text>
      ESL:IMAGE:<MAC>:<model>:<hex_encoded_buffer>
      ESL:CLEAR:<MAC>

    Responses are sent back as:
      OK:<message>
      ERR:<message>
      SCAN:<mac>:<name>:<rssi>  (one per device found)
    """
    try:
        import serial as pyserial
    except ImportError:
        raise click.ClickException("'pyserial' not installed. Run: pip install pyserial")

    click.echo(f"Opening serial port {port} at {baud} baud...")
    ser = pyserial.Serial(port, baud, timeout=1)
    click.echo("Waiting for Flipper Zero commands... (Ctrl+C to quit)")

    loop = asyncio.new_event_loop()

    try:
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            click.echo(f"< {line}")
            response = loop.run_until_complete(_handle_serial_command(line))
            click.echo(f"> {response}")
            ser.write((response + "\n").encode("utf-8"))

    except KeyboardInterrupt:
        click.echo("\nExiting serial mode.")
    finally:
        ser.close()
        loop.close()


async def _handle_serial_command(line: str) -> str:
    """Parse and execute a serial command from the Flipper Zero FAP."""
    parts = line.split(":")
    if len(parts) < 2:
        return "ERR:Unknown command"

    cmd = parts[0].upper()

    try:
        if cmd == "ESL" and parts[1].upper() == "SCAN":
            # ESL:SCAN
            devices = await BleakScanner.discover(timeout=8.0)
            responses = []
            for d in devices:
                if d.name and d.name.startswith(ESL_NAME_PREFIX):
                    rssi = getattr(d, "rssi", 0)
                    responses.append(f"SCAN:{d.address}:{d.name}:{rssi}")
            return "\n".join(responses) if responses else "OK:No devices found"

        elif cmd == "ESL" and parts[1].upper() == "PRICE":
            # ESL:PRICE:<MAC>:<model>:<price_text>:<label_text>
            if len(parts) < 6:
                return "ERR:PRICE needs MAC,model,price,label"
            mac, model_str, price_text, label = parts[2], parts[3], parts[4], parts[5]
            display_model = MODEL_MAP.get(model_str.upper(), DisplayModel.BW213)
            buf = render_price_tag(price_text, label=label, model=display_model)
            async with BleakClient(mac) as client:
                await _upload_buffer(client, buf, progress=False)
            return "OK:Price uploaded"

        elif cmd == "ESL" and parts[1].upper() == "IMAGE":
            # ESL:IMAGE:<MAC>:<model>:<hex_data>
            if len(parts) < 5:
                return "ERR:IMAGE needs MAC,model,hex_data"
            mac, model_str, hex_data = parts[2], parts[3], parts[4]
            buf = bytes.fromhex(hex_data)
            async with BleakClient(mac) as client:
                await _upload_buffer(client, buf, progress=False)
            return "OK:Image uploaded"

        elif cmd == "ESL" and parts[1].upper() == "CLEAR":
            # ESL:CLEAR:<MAC>
            if len(parts) < 3:
                return "ERR:CLEAR needs MAC"
            mac = parts[2]
            async with BleakClient(mac) as client:
                await _clear_display(client, 0xFF)
                await _write_epd(client, bytes([CMD_DISPLAY]))
            return "OK:Cleared"

        else:
            return f"ERR:Unknown command {line!r}"

    except BleakError as e:
        return f"ERR:BLE error: {e}"
    except Exception as e:
        return f"ERR:{e}"


if __name__ == "__main__":
    cli()


# ── Vusion / BT SIG ESL commands ─────────────────────────────────────────────

@cli.group()
def vusion():
    """Commands for SES-imagotag/Vusion HRD-series tags (BT SIG ESL Service, UUID 0x184D)."""


@vusion.command("scan")
@click.option("--timeout", default=12.0, show_default=True, help="Scan duration in seconds.")
def vusion_scan(timeout: float):
    """Scan for SES-imagotag/Vusion ESL tags advertising BT SIG ESL Service."""

    async def _scan():
        click.echo(f"Scanning for BT SIG ESL tags (service UUID 0x184D) for {timeout:.0f}s...")
        devices = await esl_vusion.scan_esl_tags(timeout=timeout)

        if not devices:
            click.echo(
                "No BT SIG ESL tags found.\n"
                "Tips:\n"
                "  • Make sure the tag is powered on (insert battery)\n"
                "  • Tags from eBay return to Unassociated state after ~60 min unpaired\n"
                "  • If still not found, try a broader scan with: python esl_companion.py scan"
            )
            return

        click.echo(f"\nFound {len(devices)} BT SIG ESL device(s):\n")
        click.echo(f"  {'Address':<20}  {'Name':<24}  RSSI")
        click.echo("  " + "─" * 54)
        for d in devices:
            rssi = getattr(d, "rssi", "?")
            name = d.name or "(no name)"
            click.echo(f"  {d.address:<20}  {name:<24}  {rssi} dBm")
        click.echo()
        click.echo("Use the address above with the other vusion sub-commands.")

    asyncio.run(_scan())


@vusion.command("provision")
@click.argument("address")
@click.option("--group", default=0, show_default=True, help="Group ID (0–127).")
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID (0–254).")
def vusion_provision(address: str, group: int, esl_id: int):
    """
    Provision a fresh (Unassociated) tag: bond and write mandatory characteristics.

    ADDRESS is the BLE MAC address from 'vusion scan', e.g. AA:BB:CC:DD:EE:FF

    After provisioning the tag is in Unsynchronized state and ready for commands.
    """
    async def _provision():
        click.echo(f"Looking for {address}...")
        device = await esl_vusion.BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found. Is it powered on?")
        client = await esl_vusion.connect_and_provision(
            device, group_id=group, esl_id=esl_id, verbose=True
        )
        try:
            info = await esl_vusion.read_device_info(client, esl_id=esl_id, group_id=group)
            click.echo(f"\n  Tag info: {info}")
        finally:
            await client.disconnect()

    asyncio.run(_provision())


@vusion.command("info")
@click.argument("address")
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID assigned during provisioning.")
def vusion_info(address: str, esl_id: int):
    """
    Read display info and image slots from a provisioned tag.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    """
    async def _info():
        from bleak import BleakScanner
        device = await BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found.")
        async with esl_vusion.BleakClient(device, timeout=20.0) as client:
            try:
                await client.pair()
            except Exception:
                pass
            info = await esl_vusion.read_device_info(client, esl_id=esl_id)
            click.echo(f"\nTag info: {info}")
            if info.displays:
                click.echo("\nDisplays:")
                for d in info.displays:
                    click.echo(f"  [{d.index}] {d.width}×{d.height}px  {d.color_mode}")
            click.echo(f"\nImage slots: 0 – {info.max_image_index}  ({info.max_image_index + 1} total)")

    asyncio.run(_info())


@vusion.command("display")
@click.argument("address")
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID assigned during provisioning.")
@click.option("--display-index", default=0, show_default=True, help="Display index (0 for single-display tags).")
@click.option("--image-index", default=0, show_default=True, help="Image slot to display (0 = first slot).")
def vusion_display(address: str, esl_id: int, display_index: int, image_index: int):
    """
    Display a pre-stored image on a provisioned tag.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF

    The tag stores factory images in slots 0..Max_Image_Index.  Use 'vusion info'
    to see how many image slots are available, then cycle through them to find
    pre-loaded graphics.
    """
    async def _display():
        from bleak import BleakScanner
        device = await BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found.")
        async with esl_vusion.BleakClient(device, timeout=20.0) as client:
            try:
                await client.pair()
            except Exception:
                pass
            click.echo(f"  Sending Display Image (display={display_index}, image={image_index})...")
            resp = await esl_vusion.cmd_display_image(
                client, esl_id=esl_id, display_index=display_index, image_index=image_index
            )
            if resp:
                state = esl_vusion.parse_basic_state_response(resp)
                click.echo(f"  Tag response: {state}")
            else:
                click.echo("  Command sent (no notification response).")

    asyncio.run(_display())


@vusion.command("ping")
@click.argument("address")
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID.")
def vusion_ping(address: str, esl_id: int):
    """
    Ping a provisioned tag and display its Basic State.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    """
    async def _ping():
        from bleak import BleakScanner
        device = await BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found.")
        async with esl_vusion.BleakClient(device, timeout=20.0) as client:
            try:
                await client.pair()
            except Exception:
                pass
            resp = await esl_vusion.cmd_ping(client, esl_id=esl_id)
            if resp:
                state = esl_vusion.parse_basic_state_response(resp)
                click.echo(f"Ping response: {state}")
            else:
                click.echo("Ping sent (no response).")

    asyncio.run(_ping())


@vusion.command("reset")
@click.argument("address")
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID.")
@click.option("--confirm", is_flag=True, help="Skip confirmation prompt.")
def vusion_reset(address: str, esl_id: int, confirm: bool):
    """
    Factory reset a tag — clears all provisioning data and returns to Unassociated state.

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF

    Use this to reclaim eBay tags that were previously associated with a store AP.
    After reset, re-provision with 'vusion provision'.
    """
    if not confirm:
        click.confirm(
            f"Factory reset {address}? This deletes all keys and stored images.", abort=True
        )

    async def _reset():
        from bleak import BleakScanner
        device = await BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found.")
        await esl_vusion.factory_reset_tag(device, esl_id=esl_id, verbose=True)

    asyncio.run(_reset())


@vusion.command("upload-image")
@click.argument("address")
@click.argument("image_path", type=click.Path(exists=True))
@click.option("--id", "esl_id", default=0, show_default=True, help="ESL ID.")
@click.option("--display-index", default=0, show_default=True, help="Display index.")
@click.option("--image-index", default=0, show_default=True, help="Image slot to write.")
@click.option("--width", default=0, help="Display width (0 = auto-read from tag).")
@click.option("--height", default=0, help="Display height (0 = auto-read from tag).")
@click.option("--color-mode", default="bw", type=click.Choice(["bw", "bwr"]),
              show_default=True, help="Color mode: bw (1-bit) or bwr (2-bit).")
def vusion_upload_image(
    address: str, image_path: str, esl_id: int, display_index: int,
    image_index: int, width: int, height: int, color_mode: str
):
    """
    Upload a custom image to an ESL tag via Object Transfer Protocol (OTP).

    ADDRESS is the BLE MAC address, e.g. AA:BB:CC:DD:EE:FF
    IMAGE_PATH is any image file (PNG, JPG, BMP, etc.)

    IMPORTANT: This command requires Linux with BlueZ.
    On Windows/macOS, use 'vusion display' to cycle through factory images instead.
    """
    async def _upload():
        from bleak import BleakScanner
        device = await BleakScanner.find_device_by_address(address, timeout=10.0)
        if device is None:
            raise click.ClickException(f"Device {address} not found.")

        # Read display dimensions from tag if not supplied
        w, h = width, height
        if w == 0 or h == 0:
            click.echo("  Reading display info from tag...")
            async with esl_vusion.BleakClient(device, timeout=20.0) as client:
                try:
                    await client.pair()
                except Exception:
                    pass
                info = await esl_vusion.read_device_info(client, esl_id=esl_id)
                if not info.displays:
                    raise click.ClickException("No display info found. Specify --width and --height.")
                disp = info.displays[display_index] if display_index < len(info.displays) else info.displays[0]
                w, h = disp.width, disp.height
                click.echo(f"  Display: {w}×{h}px  ({disp.color_mode})")

        click.echo(f"  Encoding image {image_path} → {w}×{h} {color_mode.upper()}...")
        if color_mode == "bw":
            img_bytes = esl_vusion.encode_image_bw(image_path, w, h)
        else:
            img_bytes = esl_vusion.encode_image_bwr(image_path, w, h)
        click.echo(f"  Encoded: {len(img_bytes)} bytes")

        click.echo(f"  Uploading to slot {image_index} via OTP (Linux/BlueZ only)...")
        try:
            ok = await esl_vusion.esl_otp_upload_linux(address, image_index, img_bytes)
            if ok:
                click.echo("  Upload complete. Sending Display Image command...")
                async with esl_vusion.BleakClient(device, timeout=20.0) as client:
                    try:
                        await client.pair()
                    except Exception:
                        pass
                    await esl_vusion.cmd_display_image(
                        client, esl_id=esl_id,
                        display_index=display_index, image_index=image_index
                    )
                click.echo("  Done.")
            else:
                raise click.ClickException("OTP upload failed.")
        except NotImplementedError as e:
            raise click.ClickException(str(e))

    asyncio.run(_upload())
