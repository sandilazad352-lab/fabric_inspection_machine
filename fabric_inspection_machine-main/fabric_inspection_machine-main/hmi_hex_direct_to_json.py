from __future__ import annotations

import argparse
import json
from pathlib import Path

from hmi_hex_to_json_fast import (
    configure_backend,
    configure_ocr,
    configure_tesseract_cmd,
    configure_value_mode,
    convert_capture,
)


def resolve_output_path(input_hex: Path, output: Path | None) -> Path:
    if output is not None:
        return output
    return input_hex.with_suffix(".json")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Directly convert HMI HEX capture to JSON using pytesseract."
    )
    parser.add_argument("input_hex", type=Path, help="Path to input .hex capture")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output JSON path (default: same file name with .json extension)",
    )
    parser.add_argument("--lang", default="eng", help="Tesseract language code")
    parser.add_argument("--psm", type=int, default=6, help="Tesseract page segmentation mode")
    parser.add_argument(
        "--ocr-backend",
        choices=("pytesseract", "easyocr"),
        default="pytesseract",
        help="OCR backend to use",
    )
    parser.add_argument(
        "--value-mode",
        choices=("strict", "balanced"),
        default="strict",
        help="strict: no guess when uncertain, balanced: choose best guess",
    )
    parser.add_argument("--tesseract-cmd", help="Full path to tesseract.exe if it is not on PATH")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation")
    args = parser.parse_args()

    if not args.input_hex.exists():
        raise SystemExit(f"Input HEX file not found: {args.input_hex}")

    if args.ocr_backend == "pytesseract":
        configure_tesseract_cmd(args.tesseract_cmd)
    configure_ocr(args.lang, args.psm)
    configure_backend(args.ocr_backend)
    configure_value_mode(args.value_mode)
    result = convert_capture(args.input_hex)
    output_path = resolve_output_path(args.input_hex, args.output)

    payload = json.dumps(result, ensure_ascii=False, indent=args.indent)
    output_path.write_text(payload, encoding="utf-8")
    print(f"JSON created: {output_path}")


if __name__ == "__main__":
    main()
