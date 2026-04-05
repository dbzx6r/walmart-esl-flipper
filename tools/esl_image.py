#!/usr/bin/env python3
"""
esl_image.py — Standalone image-to-ESL-buffer converter.

Converts any image file to the raw binary buffer format expected by
Hanshow Stellar e-ink price tags running ATC_TLSR_Paper firmware.

Usage:
    python esl_image.py input.jpg output.bin
    python esl_image.py input.jpg output.bin --model BWR213 --dither
    python esl_image.py --price "$12.99" --label "Rollback" output.bin
"""

from __future__ import annotations

import sys
from pathlib import Path

import click

try:
    from PIL import Image
except ImportError:
    click.echo("Error: Pillow not installed. Run: pip install Pillow", err=True)
    sys.exit(1)

# Allow running from parent directory without installing
sys.path.insert(0, str(Path(__file__).parent.parent / "companion"))
from image_converter import DisplayModel, DISPLAY_SIZE, image_to_esl, render_price_tag

MODEL_MAP = {m.name: m for m in DisplayModel}


@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.argument("output", type=click.Path())
@click.option("--input",  "-i", "input_path",  type=click.Path(exists=True),
              help="Input image file (JPG, PNG, BMP, etc.)")
@click.option("--price",  "-p", default=None,  help="Price text (e.g. '$12.99'). Renders a price tag layout.")
@click.option("--label",  "-l", default="",    help="Small label line above the price (used with --price).")
@click.option("--model",  "-m", default="BW213",
              type=click.Choice(list(MODEL_MAP), case_sensitive=False), show_default=True,
              help="Display model (determines resolution).")
@click.option("--dither/--no-dither", default=True, show_default=True,
              help="Apply Floyd-Steinberg dithering to grayscale images.")
@click.option("--invert", is_flag=True,
              help="Invert black and white pixels.")
@click.option("--preview", is_flag=True,
              help="Open a preview of the converted image before saving.")
@click.option("--hex",    "output_hex", is_flag=True,
              help="Output a hex dump instead of a binary file.")
def main(
    output: str,
    input_path: str | None,
    price: str | None,
    label: str,
    model: str,
    dither: bool,
    invert: bool,
    preview: bool,
    output_hex: bool,
):
    """
    Convert an image (or price text) to an ESL raw buffer file.

    Either --input or --price must be provided.

    OUTPUT is the path to write the .bin file (or hex text if --hex is given).

    Display models and their resolutions:
    \b
      BW213    → 250×122  Black/White (most common Walmart tag)
      BWR213   → 250×122  Black/White/Red
      BWR154   → 200×200  Black/White/Red 1.54"
      BW213ICE → 212×104  Black/White alt
    """
    if input_path is None and price is None:
        raise click.UsageError("Provide either --input <image_file> or --price <text>.")
    if input_path is not None and price is not None:
        raise click.UsageError("Provide either --input or --price, not both.")

    display_model = MODEL_MAP[model.upper()]
    width, height = DISPLAY_SIZE[display_model]

    if price is not None:
        click.echo(f"Rendering price: {price!r}  label: {label!r}  model: {model}  ({width}×{height})")
        buf = render_price_tag(price, label=label, model=display_model, dither=dither)
    else:
        click.echo(f"Loading image: {input_path}  →  {model} ({width}×{height})")
        src = Image.open(input_path)
        buf = image_to_esl(src, model=display_model, dither=dither, invert=invert)

    click.echo(f"Buffer size: {len(buf)} bytes")

    if preview:
        _show_preview(buf, width, height)

    if output_hex:
        hex_str = buf.hex()
        Path(output).write_text(hex_str, encoding="ascii")
        click.echo(f"Hex written to: {output}")
    else:
        Path(output).write_bytes(buf)
        click.echo(f"Binary written to: {output}")


def _show_preview(buf: bytes, width: int, height: int) -> None:
    """Reconstruct the image from the ESL buffer and display it."""
    col_bytes = (height + 7) // 8
    img = Image.new("L", (width, height), 255)

    for x in range(width):
        col_offset = (width - 1 - x) * col_bytes
        for y in range(height):
            byte_idx = y >> 3
            bit_mask = 0x80 >> (y & 7)
            if not (buf[col_offset + byte_idx] & bit_mask):
                img.putpixel((x, y), 0)  # black pixel

    img_big = img.resize((width * 3, height * 3), Image.NEAREST)
    img_big.show(title="ESL Preview")


if __name__ == "__main__":
    main()
