from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

import numpy as np

from hmi_hex_to_json_fast import (
    configure_ocr,
    configure_tesseract_cmd,
    hex_to_bitmap,
    ocr_image_variants,
    preprocess_for_ocr,
    read_hex_text,
)


def _blocks_from_easyocr_result(result: list[Any]) -> list[dict[str, Any]]:
    blocks: list[dict[str, Any]] = []
    for item in result:
        box, text, score = item
        xs = [point[0] for point in box]
        ys = [point[1] for point in box]
        blocks.append(
            {
                "text": str(text).strip(),
                "score": float(score),
                "x1": float(min(xs)),
                "y1": float(min(ys)),
                "x2": float(max(xs)),
                "y2": float(max(ys)),
                "cx": (float(min(xs)) + float(max(xs))) / 2.0,
                "cy": (float(min(ys)) + float(max(ys))) / 2.0,
            }
        )
    return blocks


def run_pytesseract(image, lang: str, psm: int) -> tuple[float, list[dict[str, Any]]]:
    configure_ocr(lang, psm)
    start = time.perf_counter()
    blocks = ocr_image_variants(image, lang=lang, psm=psm)
    elapsed = time.perf_counter() - start
    return elapsed, blocks


def run_easyocr(image, lang: str, gpu: bool) -> tuple[float, list[dict[str, Any]]]:
    import easyocr

    language_map = {
        "eng": "en",
    }
    easyocr_lang = language_map.get(lang, "en")

    reader = easyocr.Reader([easyocr_lang], gpu=gpu, verbose=False)
    image_array = np.asarray(image.convert("RGB"))

    start = time.perf_counter()
    result = reader.readtext(image_array, detail=1, paragraph=False)
    elapsed = time.perf_counter() - start
    return elapsed, _blocks_from_easyocr_result(result)


def summarize(name: str, elapsed: float, blocks: list[dict[str, Any]]) -> dict[str, Any]:
    top_blocks = sorted(blocks, key=lambda item: item["score"], reverse=True)[:10]
    return {
        "backend": name,
        "elapsed_seconds": round(elapsed, 4),
        "block_count": len(blocks),
        "top_texts": [item["text"] for item in top_blocks if item["text"]],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Benchmark OCR performance for pytesseract vs EasyOCR.")
    parser.add_argument("input_hex", type=Path, help="Path to HEX capture file")
    parser.add_argument("--lang", default="eng", help="OCR language (default: eng)")
    parser.add_argument("--psm", type=int, default=6, help="pytesseract PSM mode")
    parser.add_argument("--gpu", action="store_true", help="Enable GPU for EasyOCR")
    parser.add_argument("--tesseract-cmd", help="Optional full path to tesseract.exe")
    parser.add_argument("-o", "--output", type=Path, help="Optional JSON output file")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    args = parser.parse_args()

    configure_tesseract_cmd(args.tesseract_cmd)

    hex_text = read_hex_text(args.input_hex)
    bitmap = hex_to_bitmap(hex_text)
    image = preprocess_for_ocr(bitmap)

    tesseract_elapsed, tesseract_blocks = run_pytesseract(image, args.lang, args.psm)
    easyocr_elapsed, easyocr_blocks = run_easyocr(image, args.lang, args.gpu)

    payload: dict[str, Any] = {
        "source_file": str(args.input_hex),
        "image_size": [bitmap.width, bitmap.height],
        "pytesseract": summarize("pytesseract", tesseract_elapsed, tesseract_blocks),
        "easyocr": summarize("easyocr", easyocr_elapsed, easyocr_blocks),
        "speed_ratio_easyocr_over_pytesseract": round(
            (easyocr_elapsed / tesseract_elapsed) if tesseract_elapsed > 0 else 0.0,
            4,
        ),
    }

    output_json = json.dumps(payload, ensure_ascii=False, indent=args.indent)
    if args.output:
        args.output.write_text(output_json, encoding="utf-8")
        print(f"Performance JSON created: {args.output}")
    else:
        print(output_json)


if __name__ == "__main__":
    main()
