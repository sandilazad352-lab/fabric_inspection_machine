from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

from PIL import Image, ImageFilter, ImageOps

try:
    import pytesseract
    from pytesseract import Output
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "pytesseract is required. Install it in the workspace venv first."
    ) from exc


def set_tesseract_command(explicit_path: str | None) -> None:
    if explicit_path:
        pytesseract.pytesseract.tesseract_cmd = explicit_path
        return

    env_path = os.environ.get("TESSERACT_CMD")
    if env_path:
        pytesseract.pytesseract.tesseract_cmd = env_path
        return

    command_path = shutil.which("tesseract")
    if command_path:
        pytesseract.pytesseract.tesseract_cmd = command_path
        return

    default_windows_path = r"C:\Program Files\Tesseract-OCR\tesseract.exe"
    if Path(default_windows_path).exists():
        pytesseract.pytesseract.tesseract_cmd = default_windows_path
        return

    raise SystemExit(
        "Tesseract was not found. Install it or pass --tesseract-cmd \"C:\\Program Files\\Tesseract-OCR\\tesseract.exe\"."
    )


def preprocess_for_ocr(image: Image.Image, scale: int = 3, threshold: int = 170) -> Image.Image:
    enlarged = image.convert("L").resize(
        (image.width * scale, image.height * scale), Image.Resampling.NEAREST
    )
    contrast = ImageOps.autocontrast(enlarged)
    sharpened = contrast.filter(ImageFilter.SHARPEN)
    return sharpened.point(lambda value: 255 if value > threshold else 0)


def load_image(image_path: Path) -> Image.Image:
    if not image_path.exists():
        raise FileNotFoundError(f"Image not found: {image_path}")
    return Image.open(image_path)


def extract_words(image: Image.Image, lang: str, config: str) -> list[dict[str, Any]]:
    data = pytesseract.image_to_data(image, lang=lang, config=config, output_type=Output.DICT)
    words: list[dict[str, Any]] = []

    total = len(data.get("text", []))
    for index in range(total):
        text = str(data["text"][index]).strip()
        if not text:
            continue

        confidence_raw = data.get("conf", [])[index]
        try:
            confidence = float(confidence_raw)
        except (TypeError, ValueError):
            confidence = -1.0

        if confidence < 0:
            continue

        left = int(data["left"][index])
        top = int(data["top"][index])
        width = int(data["width"][index])
        height = int(data["height"][index])

        words.append(
            {
                "text": text,
                "confidence": confidence,
                "box": [left, top, left + width, top + height],
                "block_num": int(data["block_num"][index]),
                "par_num": int(data["par_num"][index]),
                "line_num": int(data["line_num"][index]),
                "word_num": int(data["word_num"][index]),
            }
        )

    return words


def recognize(image_path: Path, lang: str, psm: int, scale: int, threshold: int) -> dict[str, Any]:
    image = load_image(image_path)
    prepared = preprocess_for_ocr(image, scale=scale, threshold=threshold)
    config = f"--psm {psm}"

    return {
        "source_file": str(image_path),
        "image_size": [image.width, image.height],
        "preprocessed_size": [prepared.width, prepared.height],
        "full_text": pytesseract.image_to_string(prepared, lang=lang, config=config).strip(),
        "words": extract_words(prepared, lang=lang, config=config),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run pytesseract OCR on an image and export JSON.")
    parser.add_argument("input_image", type=Path, help="Path to the image file")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON to a file instead of stdout")
    parser.add_argument("--lang", default="eng", help="Tesseract language code, for example eng")
    parser.add_argument("--psm", type=int, default=6, help="Tesseract page segmentation mode")
    parser.add_argument("--scale", type=int, default=3, help="Upscale factor before OCR")
    parser.add_argument("--threshold", type=int, default=170, help="Binary threshold used during preprocessing")
    parser.add_argument(
        "--tesseract-cmd",
        help="Full path to tesseract.exe if it is not already on PATH",
    )
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    set_tesseract_command(args.tesseract_cmd)
    result = recognize(
        image_path=args.input_image,
        lang=args.lang,
        psm=args.psm,
        scale=args.scale,
        threshold=args.threshold,
    )
    payload = json.dumps(result, indent=args.indent, ensure_ascii=False)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())