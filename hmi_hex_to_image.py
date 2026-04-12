from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageOps


MARKER = "0D0A1B4BF000"
ROW_BYTES = 240
BITS_PER_BYTE = 8


def read_hex_text(path: Path) -> str:
    return "".join(path.read_text(encoding="utf-8").split())


def hex_to_bitmap(hex_text: str) -> Image.Image:
    segments = [bytes.fromhex(part) for part in hex_text.split(MARKER) if len(part) == ROW_BYTES * 2]
    if not segments:
        raise ValueError("No printer raster rows were found in the HEX capture.")

    image = Image.new("1", (ROW_BYTES, len(segments) * BITS_PER_BYTE), 1)
    for row_index, segment in enumerate(segments):
        for x_index, byte_value in enumerate(segment):
            draw_x = ROW_BYTES - 1 - x_index
            for bit_index in range(BITS_PER_BYTE):
                if not (byte_value & (1 << bit_index)):
                    image.putpixel((draw_x, row_index * BITS_PER_BYTE + bit_index), 0)
    return image


def preprocess_for_ocr(image: Image.Image, scale: int = 4) -> Image.Image:
    enlarged = image.convert("L").resize(
        (image.width * scale, image.height * scale), Image.Resampling.NEAREST
    )
    contrast = ImageOps.autocontrast(enlarged)
    return ImageOps.invert(contrast)


def resize_bitmap(image: Image.Image, scale: int) -> Image.Image:
    if scale <= 1:
        return image
    return image.resize((image.width * scale, image.height * scale), Image.Resampling.NEAREST)


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert HMI printer HEX output to image.")
    parser.add_argument("input_hex", type=Path, help="Path to HEX capture")
    parser.add_argument("-o", "--output", type=Path, default=Path("render_from_hex.png"), help="Output image path")
    parser.add_argument("--ocr-ready", action="store_true", help="Apply OCR preprocessing after upscaling")
    parser.add_argument("--scale", type=int, default=8, help="Scale used for the rendered image")
    args = parser.parse_args()

    hex_text = read_hex_text(args.input_hex)
    bitmap = hex_to_bitmap(hex_text)
    scaled = resize_bitmap(bitmap, args.scale)

    if args.ocr_ready:
        image = preprocess_for_ocr(scaled, scale=1)
    else:
        image = scaled

    image.save(args.output)
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()
