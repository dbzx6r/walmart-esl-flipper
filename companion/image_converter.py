"""
ESL Image Converter — converts any image to Hanshow Stellar e-ink buffer format.

The ATC_TLSR_Paper firmware stores images rotated 90° clockwise:
  - Buffer size: 4000 bytes for 250×122 display (250 columns × 16 bytes/col)
  - Pixel (x, y): byte = (249-x)*16 + (y>>3), bit = 0x80 >> (y&7)
  - Encoding: black=0, white=1
"""

from __future__ import annotations

from enum import Enum
from typing import Tuple

from PIL import Image, ImageDraw, ImageFont, ImageOps


class DisplayModel(Enum):
    BW213   = 1   # 250×122  Black/White
    BWR213  = 2   # 250×122  Black/White/Red
    BWR154  = 3   # 200×200  Black/White/Red
    BW213ICE = 4  # 212×104  Black/White (alternate)
    BWR350  = 5   # Larger   Black/White/Red
    BWY350  = 6   # Larger   Black/White/Yellow


# Map model → (width, height) in pixels
DISPLAY_SIZE: dict[DisplayModel, Tuple[int, int]] = {
    DisplayModel.BW213:    (250, 122),
    DisplayModel.BWR213:   (250, 122),
    DisplayModel.BWR154:   (200, 200),
    DisplayModel.BW213ICE: (212, 104),
    DisplayModel.BWR350:   (250, 128),  # approximate
    DisplayModel.BWY350:   (250, 128),  # approximate
}

# bytes per column = ceil(height / 8)
def _col_bytes(height: int) -> int:
    return (height + 7) // 8

# total buffer size for a display model
def buffer_size(model: DisplayModel) -> int:
    w, h = DISPLAY_SIZE[model]
    return w * _col_bytes(h)


def image_to_esl(
    img: Image.Image,
    model: DisplayModel = DisplayModel.BW213,
    dither: bool = True,
    invert: bool = False,
) -> bytes:
    """
    Convert a PIL Image to the raw ESL buffer format.

    Args:
        img:     Input image (any mode/size — will be resized + converted).
        model:   Target display model (determines resolution).
        dither:  If True, apply Floyd-Steinberg dithering for better grayscale rendering.
        invert:  If True, invert black/white (for dark-background images).

    Returns:
        Raw bytes ready to upload to the tag (length = buffer_size(model)).
    """
    width, height = DISPLAY_SIZE[model]
    col_bytes = _col_bytes(height)

    # Convert to grayscale and resize to fit display (maintain aspect ratio with padding)
    img = img.convert("RGBA").convert("RGB")
    img = _fit_image(img, width, height)

    # Convert to grayscale
    img = img.convert("L")

    if invert:
        img = ImageOps.invert(img)

    # Convert to 1-bit (with or without dithering)
    if dither:
        img = img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    else:
        img = img.point(lambda p: 255 if p >= 128 else 0, "1")

    # Build the ESL buffer
    # Buffer is stored rotated 90° clockwise:
    #   column k in memory = display column x = (width-1-k)
    #   row within column = y
    buf = bytearray(b"\xff" * (width * col_bytes))  # start all-white

    for x in range(width):
        col_offset = (width - 1 - x) * col_bytes
        for y in range(height):
            pixel = img.getpixel((x, y))
            # In PIL 1-bit images: 0=black, 255 (or True)=white
            is_black = (pixel == 0)
            if is_black:
                byte_idx = y >> 3
                bit_mask = 0x80 >> (y & 7)
                buf[col_offset + byte_idx] &= ~bit_mask  # clear bit = black

    return bytes(buf)


def _fit_image(img: Image.Image, width: int, height: int) -> Image.Image:
    """Resize image to (width, height), padding with white to preserve aspect ratio."""
    # Copy so we don't modify the caller's image
    resized = img.copy()
    resized.thumbnail((width, height), Image.LANCZOS)
    # Create white canvas and paste centered
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    paste_x = (width - resized.width) // 2
    paste_y = (height - resized.height) // 2
    canvas.paste(resized, (paste_x, paste_y))
    return canvas


def render_price_tag(
    price: str,
    label: str = "",
    model: DisplayModel = DisplayModel.BW213,
    dither: bool = False,
) -> bytes:
    """
    Render a price tag layout to ESL buffer format.

    Renders a large price string with an optional smaller label line above it.
    Uses a built-in bitmap approach for maximum compatibility.

    Args:
        price:  Price string, e.g. "$12.99" or "SALE"
        label:  Optional small text above the price, e.g. "Great Value"
        model:  Target display model.
        dither: Apply dithering (usually False for text).

    Returns:
        Raw bytes for the ESL buffer.
    """
    width, height = DISPLAY_SIZE[model]
    img = Image.new("L", (width, height), 255)  # white background
    draw = ImageDraw.Draw(img)

    # Try to load a system font; fall back to default if unavailable
    price_font = _load_font(size=min(width // max(len(price), 3), 72))
    label_font = _load_font(size=16)

    # Draw label (small, top)
    if label:
        draw.text((4, 4), label, fill=0, font=label_font)
        price_y = 28
    else:
        price_y = (height - _font_height(price_font)) // 2

    # Draw price (large, centered horizontally)
    bbox = draw.textbbox((0, 0), price, font=price_font)
    text_w = bbox[2] - bbox[0]
    price_x = max(2, (width - text_w) // 2)
    draw.text((price_x, price_y), price, fill=0, font=price_font)

    # Add a thin border
    draw.rectangle([(0, 0), (width - 1, height - 1)], outline=0, width=2)

    return image_to_esl(img, model=model, dither=dither)


def _load_font(size: int):
    """Load a reasonably large font, falling back gracefully."""
    # Try common system fonts in order of preference
    candidates = [
        "arialbd.ttf",   # Arial Bold (Windows)
        "Arial Bold.ttf",
        "DejaVuSans-Bold.ttf",  # Linux
        "Helvetica-Bold.ttf",   # macOS
        "LiberationSans-Bold.ttf",
    ]
    for name in candidates:
        try:
            return ImageFont.truetype(name, size)
        except (IOError, OSError):
            continue
    # Ultimate fallback — PIL default bitmap font
    return ImageFont.load_default()


def _font_height(font) -> int:
    """Get approximate font height."""
    try:
        bbox = font.getbbox("A")
        return bbox[3] - bbox[1]
    except AttributeError:
        return 10
