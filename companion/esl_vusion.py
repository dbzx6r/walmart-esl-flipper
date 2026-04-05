#!/usr/bin/env python3
"""
ESL Vusion — BLE client for SES-imagotag / Vusion HRD-series e-ink price tags.

These tags (e.g., HRD3-0210-A) use the Qualcomm QCC710 BLE 5.3 chip and implement
the Bluetooth SIG Electronic Shelf Label Service (UUID 0x184D), published March 2023.
See: https://www.bluetooth.com/specifications/specs/electronic-shelf-label-profile-1-0/

Protocol overview
-----------------
Tags operate as a BLE GATT server. The AP (Access Point, i.e. this script) acts as
the GATT client.

State machine (relevant subset):
  Unassociated  → tag advertises ESL Service UUID, accepts bonding
  Configuring   → tag is bonded; AP writes 4 mandatory characteristics:
                    ESL Address, AP Sync Key Material,
                    ESL Response Key Material, ESL Current Absolute Time
  Unsynchronized→ provisioning complete; tag ready for commands

Commands (ESL Control Point, TLV format):
  Opcode byte: upper nibble = length (L), lower nibble = tag (T)
    Total TLV size = L + 2 octets  (1 opcode + (L+1) parameter bytes)
  First parameter byte is always ESL_ID.

  0x00  Ping             — solicits a Basic State response
  0x01  Unassociate       — remove bonding, return to Unassociated state
  0x02  Service Reset     — clear the Service Needed flag
  0x03  Factory Reset     — factory reset (delete all provisioning data)
  0x04  Update Complete   — return to Synchronized state after update
  0x11  Refresh Display   — re-draw current image (ESL_ID, Display_Index)
  0x20  Display Image     — show stored image (ESL_ID, Display_Index, Image_Index)

Image upload (Object Transfer Profile, OTP / OTS service 0x1825):
  Uploading images requires LE L2CAP Credit-Based connections (CoC), which is
  supported by BlueZ on Linux (not by Windows/macOS BLE stacks via bleak).
  A helper function esl_otp_upload() is provided for Linux/BlueZ.

All characteristics require an encrypted (bonded) BLE connection.

Characteristic UUIDs (Bluetooth SIG Assigned Numbers, 2024):
  ESL Address              0x2BF6  (write, encrypted)
  AP Sync Key Material     0x2BF7  (write, encrypted)
  ESL Response Key Material 0x2BF8 (write, encrypted)
  ESL Current Absolute Time 0x2BF9 (write, encrypted)
  ESL Display Information   0x2BFA (read,  encrypted)
  ESL Image Information     0x2BFB (read,  encrypted)
  ESL Sensor Information    0x2BFC (read,  encrypted)
  ESL LED Information       0x2BFD (read,  encrypted)
  ESL Control Point         0x2BFE (write+notify, encrypted)

Object Transfer Service:
  OTS Service UUID          0x1825
  Object Action Ctrl Pt     0x2AC5  (write+indicate)
  Object List Ctrl Pt       0x2AC6  (write+indicate)
  Object ID                 0x2AC3  (read)
  Object Size               0x2AC0  (read)
  OACP Image Object Base ID 0x000000000100 + Image_Index
"""

from __future__ import annotations

import asyncio
import os
import platform
import struct
import sys
import time
from typing import Optional

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
    from bleak.exc import BleakError
except ImportError:
    print("Error: 'bleak' not installed. Run: pip install bleak", file=sys.stderr)
    sys.exit(1)

# ── BT SIG ESL Service / Characteristic UUIDs ────────────────────────────────
#
# All are 16-bit UUIDs expanded to 128-bit using the standard BT SIG base UUID:
#   0000XXXX-0000-1000-8000-00805f9b34fb

ESL_SERVICE_UUID = "0000184d-0000-1000-8000-00805f9b34fb"

ESL_CHAR_ADDRESS       = "00002bf6-0000-1000-8000-00805f9b34fb"
ESL_CHAR_AP_SYNC_KEY   = "00002bf7-0000-1000-8000-00805f9b34fb"
ESL_CHAR_RESP_KEY      = "00002bf8-0000-1000-8000-00805f9b34fb"
ESL_CHAR_ABS_TIME      = "00002bf9-0000-1000-8000-00805f9b34fb"
ESL_CHAR_DISPLAY_INFO  = "00002bfa-0000-1000-8000-00805f9b34fb"
ESL_CHAR_IMAGE_INFO    = "00002bfb-0000-1000-8000-00805f9b34fb"
ESL_CHAR_SENSOR_INFO   = "00002bfc-0000-1000-8000-00805f9b34fb"
ESL_CHAR_LED_INFO      = "00002bfd-0000-1000-8000-00805f9b34fb"
ESL_CHAR_CONTROL_POINT = "00002bfe-0000-1000-8000-00805f9b34fb"

# Object Transfer Service (for image upload)
OTS_SERVICE_UUID   = "00001825-0000-1000-8000-00805f9b34fb"
OTS_CHAR_OBJ_ID    = "00002ac3-0000-1000-8000-00805f9b34fb"
OTS_CHAR_OBJ_SIZE  = "00002ac0-0000-1000-8000-00805f9b34fb"
OTS_CHAR_OACP      = "00002ac5-0000-1000-8000-00805f9b34fb"  # Action Control Point
OTS_CHAR_OLCP      = "00002ac6-0000-1000-8000-00805f9b34fb"  # List Control Point

# ESL Control Point TLV opcodes
#  Opcode byte: upper nibble = L (parameter count = L+1), lower nibble = T (command tag)
ESL_OP_PING            = 0x00   # T=0, L=0 → params: [ESL_ID]
ESL_OP_UNASSOCIATE     = 0x01   # T=1, L=0 → params: [ESL_ID]
ESL_OP_SERVICE_RESET   = 0x02   # T=2, L=0 → params: [ESL_ID]
ESL_OP_FACTORY_RESET   = 0x03   # T=3, L=0 → params: [ESL_ID]
ESL_OP_UPDATE_COMPLETE = 0x04   # T=4, L=0 → params: [ESL_ID]
ESL_OP_REFRESH_DISPLAY = 0x11   # T=1, L=1 → params: [ESL_ID, Display_Index]
ESL_OP_DISPLAY_IMAGE   = 0x20   # T=0, L=2 → params: [ESL_ID, Display_Index, Image_Index]

# ESL Control Point response opcodes
ESL_RESP_BASIC_STATE   = 0x10   # T=0, L=1 → [ESL_ID, Basic_State(1B)]
ESL_RESP_ERROR         = 0x11   # T=1, L=1 → [ESL_ID, Error_Code(1B)]

# Display type codes (Bluetooth SIG Assigned Numbers §18)
DISPLAY_TYPE_BLACK_WHITE          = 0x01
DISPLAY_TYPE_THREE_COLOR_BWR      = 0x02  # Black + White + Red
DISPLAY_TYPE_THREE_COLOR_BWY      = 0x03  # Black + White + Yellow
DISPLAY_TYPE_FOUR_COLOR           = 0x04

# OTP OACP opcodes
OACP_OP_WRITE  = 0x03
OACP_OP_GOTO   = 0x01  # (OLCP GoTo command)
OACP_RESP_CODE = 0x60
OACP_SUCCESS   = 0x01

# Image object ID base (48-bit): base + Image_Index
OBJ_ID_BASE = 0x000000000100

# ESL_ID broadcast address
ESL_ID_BROADCAST = 0xFF


# ── Data structures ───────────────────────────────────────────────────────────

class EslDisplayInfo:
    """Parsed ESL Display Information (one entry per display)."""

    def __init__(self, width: int, height: int, display_type: int, index: int = 0):
        self.index = index
        self.width = width
        self.height = height
        self.display_type = display_type

    @property
    def color_mode(self) -> str:
        return {
            DISPLAY_TYPE_BLACK_WHITE:     "Black/White",
            DISPLAY_TYPE_THREE_COLOR_BWR: "Black/White/Red",
            DISPLAY_TYPE_THREE_COLOR_BWY: "Black/White/Yellow",
            DISPLAY_TYPE_FOUR_COLOR:      "4-color",
        }.get(self.display_type, f"Unknown (0x{self.display_type:02X})")

    def __repr__(self) -> str:
        return (
            f"Display[{self.index}]: {self.width}×{self.height}px "
            f"({self.color_mode})"
        )


class EslDeviceInfo:
    """Information read from a provisioned ESL tag."""

    def __init__(self):
        self.displays: list[EslDisplayInfo] = []
        self.max_image_index: int = 0
        self.esl_id: int = 0
        self.group_id: int = 0

    def __repr__(self) -> str:
        disp = ", ".join(repr(d) for d in self.displays)
        return (
            f"ESL(group={self.group_id}, id={self.esl_id}, "
            f"images=0..{self.max_image_index}, displays=[{disp}])"
        )


# ── Scan ──────────────────────────────────────────────────────────────────────

async def scan_esl_tags(timeout: float = 10.0) -> list[BLEDevice]:
    """
    Scan for BLE devices advertising the BT SIG ESL Service UUID (0x184D).

    Returns a list of BLEDevice objects for each ESL tag found.
    """
    found: list[BLEDevice] = []

    def _filter(device: BLEDevice, adv_data) -> bool:
        if adv_data.service_uuids:
            for uuid in adv_data.service_uuids:
                if uuid.lower() == ESL_SERVICE_UUID.lower():
                    return True
        # Some tags only advertise the short 16-bit UUID; check for 184d in name too
        if device.name and ("esl" in device.name.lower() or "vusion" in device.name.lower()):
            return True
        return False

    device = await BleakScanner.find_device_by_filter(_filter, timeout=timeout)
    if device:
        found.append(device)

    # Also do a full scan and filter
    all_devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for dev, adv in all_devices.values():
        if _filter(dev, adv) and dev.address not in {d.address for d in found}:
            found.append(dev)

    return found


# ── ESL Address encoding ──────────────────────────────────────────────────────

def encode_esl_address(group_id: int, esl_id: int) -> bytes:
    """
    Encode a (group_id, esl_id) pair as a 2-byte ESL Address (little-endian uint16).

    ESL Address format (15-bit value):
      Bits [14:8] = Group_ID (7 bits, 0–127)
      Bits [7:0]  = ESL_ID  (8 bits, 0–254; 0xFF is broadcast)
    """
    if not (0 <= group_id <= 127):
        raise ValueError(f"group_id must be 0–127, got {group_id}")
    if not (0 <= esl_id <= 0xFE):
        raise ValueError(f"esl_id must be 0–254, got {esl_id}")
    value = (group_id << 8) | esl_id
    return struct.pack("<H", value)


# ── Connect / provision ───────────────────────────────────────────────────────

async def connect_and_provision(
    device: BLEDevice,
    group_id: int = 0,
    esl_id: int = 0,
    verbose: bool = True,
) -> BleakClient:
    """
    Connect to an ESL tag in Unassociated state, bond with it, and write the
    4 mandatory provisioning characteristics.

    After this call the tag will be in Unsynchronized state and ready to receive
    Control Point commands.

    Parameters
    ----------
    device    : BLEDevice returned from scan_esl_tags()
    group_id  : 7-bit Group_ID to assign (0–127). Use 0 unless running multiple groups.
    esl_id    : 8-bit ESL_ID to assign (0–254). Must be unique within the group.
    verbose   : print progress messages

    Returns
    -------
    A connected, bonded BleakClient. Caller is responsible for disconnecting.
    """

    def _log(msg: str) -> None:
        if verbose:
            print(f"  {msg}")

    _log(f"Connecting to {device.name or device.address} ({device.address})...")

    # pair=True causes bleak to initiate BLE bonding (required by ESL spec)
    client = BleakClient(device, timeout=30.0)
    await client.connect()

    _log("Connected. Initiating BLE bonding...")

    # Request pairing/bonding.  On Windows the OS handles this automatically
    # (Just Works pairing will succeed without a PIN for an Unassociated tag).
    # On Linux with BlueZ, bluetoothd handles the bonding procedure.
    try:
        await client.pair()
        _log("Bonded successfully.")
    except Exception as exc:
        # Some platforms (macOS CoreBluetooth) don't expose explicit pair() —
        # bonding happens implicitly when writing an encrypted characteristic.
        _log(f"pair() returned: {exc}  (this may be normal on macOS/Linux)")

    _log("Writing provisioning characteristics...")

    # 1. ESL Address (2 bytes, little-endian uint16)
    esl_addr_bytes = encode_esl_address(group_id, esl_id)
    await client.write_gatt_char(ESL_CHAR_ADDRESS, esl_addr_bytes, response=True)
    _log(f"  ESL Address     → group={group_id}, id={esl_id}  [{esl_addr_bytes.hex()}]")

    # 2. AP Sync Key Material (24 random bytes)
    ap_sync_key = os.urandom(24)
    await client.write_gatt_char(ESL_CHAR_AP_SYNC_KEY, ap_sync_key, response=True)
    _log(f"  AP Sync Key     → {ap_sync_key.hex()}")

    # 3. ESL Response Key Material (24 random bytes)
    resp_key = os.urandom(24)
    await client.write_gatt_char(ESL_CHAR_RESP_KEY, resp_key, response=True)
    _log(f"  Response Key    → {resp_key.hex()}")

    # 4. ESL Current Absolute Time (ms since arbitrary epoch, uint32 LE)
    #    The spec doesn't mandate an epoch — we use ms since process start.
    abs_time = int(time.monotonic() * 1000) & 0xFFFFFFFF
    await client.write_gatt_char(
        ESL_CHAR_ABS_TIME, struct.pack("<I", abs_time), response=True
    )
    _log(f"  Absolute Time   → {abs_time} ms  [{struct.pack('<I', abs_time).hex()}]")

    _log("Provisioning complete. Tag is now in Unsynchronized state.")
    return client


# ── Read device info ──────────────────────────────────────────────────────────

async def read_device_info(client: BleakClient, esl_id: int = 0, group_id: int = 0) -> EslDeviceInfo:
    """
    Read ESL Display Information and ESL Image Information from a provisioned tag.
    """
    info = EslDeviceInfo()
    info.esl_id = esl_id
    info.group_id = group_id

    try:
        disp_data = await client.read_gatt_char(ESL_CHAR_DISPLAY_INFO)
        # Each Display Data Structure is 5 bytes: Width(2) + Height(2) + Display_Type(1)
        n_displays = len(disp_data) // 5
        for i in range(n_displays):
            offset = i * 5
            w, h, dtype = struct.unpack_from("<HHB", disp_data, offset)
            info.displays.append(EslDisplayInfo(w, h, dtype, index=i))
    except Exception:
        pass  # characteristic not present (no display)

    try:
        img_data = await client.read_gatt_char(ESL_CHAR_IMAGE_INFO)
        info.max_image_index = img_data[0]
    except Exception:
        pass  # characteristic not present (no image storage)

    return info


# ── Control Point commands ────────────────────────────────────────────────────

async def _write_control_point(client: BleakClient, payload: bytes, timeout: float = 5.0) -> Optional[bytes]:
    """
    Write a TLV command to the ESL Control Point characteristic and wait for
    the notification response (if CCCD is enabled).

    Returns the raw notification payload, or None if no notification arrives.
    """
    response_event = asyncio.Event()
    response_data: list[bytes] = []

    def _notification_handler(_sender, data: bytes) -> None:
        response_data.append(bytes(data))
        response_event.set()

    try:
        await client.start_notify(ESL_CHAR_CONTROL_POINT, _notification_handler)
        notify_enabled = True
    except Exception:
        notify_enabled = False

    await client.write_gatt_char(ESL_CHAR_CONTROL_POINT, payload, response=True)

    if notify_enabled:
        try:
            await asyncio.wait_for(response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
        try:
            await client.stop_notify(ESL_CHAR_CONTROL_POINT)
        except Exception:
            pass

    return response_data[0] if response_data else None


def _build_cmd(opcode: int, *params: int) -> bytes:
    """Build a TLV command payload: [opcode, *params]."""
    return bytes([opcode] + list(params))


async def cmd_ping(client: BleakClient, esl_id: int = 0) -> Optional[bytes]:
    """Send Ping command and return the Basic State response (2 bytes)."""
    return await _write_control_point(client, _build_cmd(ESL_OP_PING, esl_id))


async def cmd_display_image(
    client: BleakClient,
    esl_id: int = 0,
    display_index: int = 0,
    image_index: int = 0,
) -> Optional[bytes]:
    """
    Command the tag to display a pre-stored image.

    image_index must be <= Max_Image_Index read from ESL Image Information.
    Freshly provisioned tags have factory images in slots 0..Max_Image_Index.
    """
    payload = _build_cmd(ESL_OP_DISPLAY_IMAGE, esl_id, display_index, image_index)
    return await _write_control_point(client, payload)


async def cmd_refresh_display(
    client: BleakClient,
    esl_id: int = 0,
    display_index: int = 0,
) -> Optional[bytes]:
    """Refresh (re-draw) the current image on the display."""
    payload = _build_cmd(ESL_OP_REFRESH_DISPLAY, esl_id, display_index)
    return await _write_control_point(client, payload)


async def cmd_factory_reset(client: BleakClient, esl_id: int = 0) -> None:
    """
    Factory reset the tag.

    This removes all provisioning data (keys, ESL Address, stored images) and
    returns the tag to Unassociated state.  No response is sent by the tag.
    """
    payload = _build_cmd(ESL_OP_FACTORY_RESET, esl_id)
    await client.write_gatt_char(ESL_CHAR_CONTROL_POINT, payload, response=True)


async def cmd_unassociate(client: BleakClient, esl_id: int = 0) -> Optional[bytes]:
    """Disassociate the tag from this AP (removes bonding + sync key)."""
    payload = _build_cmd(ESL_OP_UNASSOCIATE, esl_id)
    return await _write_control_point(client, payload)


async def cmd_update_complete(client: BleakClient, esl_id: int = 0) -> None:
    """Signal that the update session is complete."""
    payload = _build_cmd(ESL_OP_UPDATE_COMPLETE, esl_id)
    await client.write_gatt_char(ESL_CHAR_CONTROL_POINT, payload, response=True)


def parse_basic_state_response(resp: bytes) -> dict:
    """
    Parse a Basic State response notification from the ESL Control Point.

    Returns a dict with keys:
      opcode, esl_id, service_needed, synchronized, low_battery, display_update
    """
    if not resp or len(resp) < 3:
        return {"raw": resp.hex() if resp else ""}

    opcode, esl_id, state_byte = resp[0], resp[1], resp[2]
    return {
        "opcode":         f"0x{opcode:02X}",
        "esl_id":         esl_id,
        "service_needed": bool(state_byte & 0x01),
        "synchronized":   bool(state_byte & 0x02),
        "low_battery":    bool(state_byte & 0x04),
        "display_update": bool(state_byte & 0x08),
    }


# ── Image upload via Object Transfer Protocol (OTP) ──────────────────────────
#
# Image upload requires LE L2CAP Credit-Based Connections (CoC), which
# are only supported via BlueZ on Linux. On Windows/macOS, bleak does not
# provide L2CAP CoC access.
#
# Image format: The BT SIG ESL spec leaves the image encoding vendor-defined.
# SES-imagotag (Vusion) likely uses a raw packed-pixel format consistent with
# the Display_Type from ESL Display Information:
#   - BW (type 0x01): 1 bit/pixel, row-major, MSB first
#   - BWR/BWY (type 0x02/0x03): 2 bits/pixel, row-major, packed
#
# The OTP image upload procedure:
#   1. Connect OTS (service 0x1825)
#   2. OLCP GoTo: select object by 48-bit ID = 0x000000000100 + Image_Index
#   3. OACP Write: offset=0, length=image_data_length
#   4. Transfer image bytes over L2CAP PSM 0x0025 (OTS CoC channel)
#
# A Linux/BlueZ implementation using PyBluez or socket(AF_BLUETOOTH, SOCK_SEQPACKET)
# is required for full image upload support.

async def esl_otp_upload_linux(
    device_address: str,
    image_index: int,
    image_data: bytes,
) -> bool:
    """
    Upload an image to an ESL tag using the Object Transfer Protocol.

    **Requires Linux with BlueZ.**  Uses a raw L2CAP CoC socket to transfer
    the image data.  On Windows/macOS, this function raises NotImplementedError.

    Parameters
    ----------
    device_address : BLE MAC address string, e.g. "AA:BB:CC:DD:EE:FF"
    image_index    : Image slot to write (0 = first slot)
    image_data     : Raw packed-pixel image bytes (see format notes above)

    Returns True on success, False on failure.
    """
    if platform.system() != "Linux":
        raise NotImplementedError(
            "OTP image upload via L2CAP CoC is only supported on Linux with BlueZ.\n"
            "On Windows/macOS, use the factory images already stored on the tag\n"
            "by cycling through image slots with cmd_display_image()."
        )

    import socket
    import ctypes

    # Object ID for this image slot
    obj_id = OBJ_ID_BASE + image_index  # 48-bit value

    # First, use GATT to initiate the write via OACP
    async with BleakClient(device_address) as client:
        # OLCP GoTo — select the target object by ID
        # GoTo opcode = 0x03, parameter = 6-byte little-endian Object ID
        olcp_goto = bytes([0x03]) + struct.pack("<Q", obj_id)[:6]

        # Wait for OLCP response indication
        event = asyncio.Event()
        olcp_resp: list[bytes] = []

        def _olcp_handler(_sender, data: bytes) -> None:
            olcp_resp.append(bytes(data))
            event.set()

        await client.start_notify(OTS_CHAR_OLCP, _olcp_handler)
        await client.write_gatt_char(OTS_CHAR_OLCP, olcp_goto, response=True)
        try:
            await asyncio.wait_for(event.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            print("  WARNING: OLCP GoTo timed out — object may still be selected")

        await client.stop_notify(OTS_CHAR_OLCP)

        # OACP Write — request write of len(image_data) bytes at offset 0
        # OACP Write opcode = 0x03, params: Offset(4B LE) + Length(4B LE)
        oacp_write = bytes([0x03]) + struct.pack("<II", 0, len(image_data))

        event2 = asyncio.Event()
        oacp_resp: list[bytes] = []

        def _oacp_handler(_sender, data: bytes) -> None:
            oacp_resp.append(bytes(data))
            event2.set()

        await client.start_notify(OTS_CHAR_OACP, _oacp_handler)
        await client.write_gatt_char(OTS_CHAR_OACP, oacp_write, response=True)
        try:
            await asyncio.wait_for(event2.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            print("  WARNING: OACP Write response timed out")
            await client.stop_notify(OTS_CHAR_OACP)
            return False

        await client.stop_notify(OTS_CHAR_OACP)

        if oacp_resp:
            resp = oacp_resp[0]
            # Response format: [Resp_Opcode(0x60), Req_Opcode, Result_Code]
            if len(resp) >= 3 and resp[0] == OACP_RESP_CODE:
                if resp[2] != OACP_SUCCESS:
                    print(f"  ERROR: OACP Write rejected with code 0x{resp[2]:02X}")
                    return False

    # Now send image data over L2CAP CoC PSM 0x0025 (OTS)
    # AF_BLUETOOTH = 31, BTPROTO_L2CAP = 0
    AF_BLUETOOTH = 31
    BTPROTO_L2CAP = 0
    OTS_L2CAP_PSM = 0x0025

    addr = device_address.upper()
    try:
        sock = socket.socket(AF_BLUETOOTH, socket.SOCK_SEQPACKET, BTPROTO_L2CAP)
        sock.settimeout(10.0)

        # sockaddr_l2 structure for Linux BlueZ
        # struct sockaddr_l2 { sa_family_t l2_family; __le16 l2_psm; bdaddr_t l2_bdaddr; __le16 l2_cid; uint8_t l2_bdaddr_type; }
        mac_bytes = bytes(int(b, 16) for b in reversed(addr.split(":")))
        sockaddr = struct.pack("=HH6sHB", AF_BLUETOOTH, socket.htons(OTS_L2CAP_PSM), mac_bytes, 0, 1)

        sock.connect(sockaddr)
        chunk_size = 512
        offset = 0
        total = len(image_data)
        while offset < total:
            chunk = image_data[offset:offset + chunk_size]
            sock.send(chunk)
            offset += len(chunk)
            pct = int(offset / total * 100)
            print(f"\r  OTP upload: {pct}% ({offset}/{total} bytes)", end="", flush=True)
        print()
        sock.close()
        print("  OTP image upload complete.")
        return True

    except Exception as exc:
        print(f"  ERROR: L2CAP CoC transfer failed: {exc}")
        return False


# ── Image encoding helpers ────────────────────────────────────────────────────

def encode_image_bw(image_path: str, width: int, height: int) -> bytes:
    """
    Convert an image file to a 1-bit/pixel packed bitmap for BW ESL displays.

    Layout: row-major, MSB first (pixel 0 = MSB of byte 0).
    Black pixel = 0, White pixel = 1.

    Returns raw bytes of length ceil(width * height / 8).
    """
    try:
        from PIL import Image
    except ImportError:
        raise ImportError("Pillow is required for image encoding: pip install Pillow")

    img = Image.open(image_path).convert("L")  # Greyscale
    img = img.resize((width, height), Image.LANCZOS)
    img = img.convert("1")  # 1-bit with dithering

    row_bytes = (width + 7) // 8
    buf = bytearray(row_bytes * height)

    for y in range(height):
        for x in range(width):
            px = img.getpixel((x, y))
            # PIL mode "1": 0 = black, 255 = white (or True/False)
            is_white = (px != 0)
            byte_idx = y * row_bytes + (x // 8)
            bit_pos = 7 - (x % 8)  # MSB first
            if is_white:
                buf[byte_idx] |= (1 << bit_pos)

    return bytes(buf)


def encode_image_bwr(image_path: str, width: int, height: int) -> bytes:
    """
    Convert an image file to a 2-bit/pixel packed bitmap for BWR (3-color) ESL displays.

    Layout: row-major, MSB first.
    Bit encoding per pixel: 00 = black, 01 = white, 10 = red, 11 = (unused)

    Returns raw bytes of length ceil(width * height / 4).
    """
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        raise ImportError("Pillow and numpy are required: pip install Pillow numpy")

    img = Image.open(image_path).convert("RGB")
    img = img.resize((width, height), Image.LANCZOS)
    arr = np.array(img, dtype=np.float32)

    # Quantize to 3 colors: black (0), white (1), red (2)
    # Distance to each color in RGB space
    colors = np.array([[0, 0, 0], [255, 255, 255], [220, 0, 0]], dtype=np.float32)
    pixels = arr.reshape(-1, 3)
    diffs = pixels[:, None, :] - colors[None, :, :]
    color_idx = np.argmin(np.sum(diffs ** 2, axis=2), axis=1)  # 0/1/2 per pixel

    row_bits = width * 2  # 2 bits per pixel
    row_bytes = (row_bits + 7) // 8
    buf = bytearray(row_bytes * height)

    for idx, c in enumerate(color_idx):
        y = idx // width
        x = idx % width
        byte_idx = y * row_bytes + (x * 2) // 8
        bit_offset = 6 - (x * 2) % 8  # MSB first, 2 bits
        buf[byte_idx] |= (int(c) & 0x03) << bit_offset

    return bytes(buf)


# ── High-level convenience API ────────────────────────────────────────────────

async def provision_and_display(
    device: BLEDevice,
    image_index: int = 0,
    display_index: int = 0,
    group_id: int = 0,
    esl_id: int = 0,
    verbose: bool = True,
) -> None:
    """
    Provision a fresh ESL tag and immediately display image at image_index.

    This uses factory-loaded images that are already in the tag's flash.
    For custom image upload, use esl_otp_upload_linux() on Linux/BlueZ.
    """
    client = await connect_and_provision(device, group_id=group_id, esl_id=esl_id, verbose=verbose)
    try:
        if verbose:
            info = await read_device_info(client, esl_id=esl_id, group_id=group_id)
            print(f"  Tag info: {info}")

        if verbose:
            print(f"  Sending Display Image (display={display_index}, image={image_index})...")
        resp = await cmd_display_image(client, esl_id=esl_id,
                                       display_index=display_index,
                                       image_index=image_index)
        if resp and verbose:
            state = parse_basic_state_response(resp)
            print(f"  Tag response: {state}")

        if verbose:
            print("  Done.")
    finally:
        await client.disconnect()


async def factory_reset_tag(device: BLEDevice, esl_id: int = 0, verbose: bool = True) -> None:
    """
    Factory reset an ESL tag (must be bonded/provisioned).

    Returns the tag to Unassociated state.  Useful for resetting eBay tags
    that were previously paired with a store AP.
    """
    if verbose:
        print(f"  Connecting to {device.name or device.address}...")

    async with BleakClient(device, timeout=30.0) as client:
        if verbose:
            print("  Connected. Sending Factory Reset...")
        try:
            await client.pair()
        except Exception:
            pass
        await cmd_factory_reset(client, esl_id=esl_id)
        if verbose:
            print("  Factory reset sent. Tag will disconnect and return to Unassociated state.")
