from __future__ import annotations

import argparse
import json
import re
import sys
from functools import lru_cache
from pathlib import Path
from difflib import SequenceMatcher
from collections import defaultdict
from typing import Any

import numpy as np

try:
    PROJECT_ROOT = Path(__file__).resolve().parent
except NameError:  # pragma: no cover - supports exec() in the active REPL
    PROJECT_ROOT = Path.cwd()
VENV_SITE_PACKAGES = PROJECT_ROOT / ".venv" / "Lib" / "site-packages"
if VENV_SITE_PACKAGES.exists() and str(VENV_SITE_PACKAGES) not in sys.path:
    sys.path.insert(0, str(VENV_SITE_PACKAGES))
from PIL import Image, ImageOps

try:
    from rapidocr_onnxruntime import RapidOCR
except ImportError as exc:  # pragma: no cover - dependency guard
    raise SystemExit(
        "rapidocr-onnxruntime is required. Install it in the workspace venv or "
        "add it to the active Python environment."
    ) from exc

MARKER = "0D0A1B4BF000"
ROW_BYTES = 240
BITS_PER_BYTE = 8

@lru_cache(maxsize=1)
def get_ocr_engine() -> RapidOCR:
    return RapidOCR()

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

def preprocess_for_ocr(image: Image.Image) -> Image.Image:
    enlarged = image.convert("L").resize((image.width * 4, image.height * 4), Image.Resampling.NEAREST)
    contrast = ImageOps.autocontrast(enlarged)
    return ImageOps.invert(contrast)

def image_to_ocr_array(image: Image.Image) -> np.ndarray:
    return np.asarray(image.convert("RGB"))

def ocr_image(image: Image.Image) -> list[dict[str, Any]]:
    ocr = get_ocr_engine()
    result, _ = ocr(image_to_ocr_array(image), use_cls=False, use_rec=True)

    parsed: list[dict[str, Any]] = []
    for box, text, score in result or []:
        xs = [point[0] for point in box]
        ys = [point[1] for point in box]
        parsed.append(
            {
                "text": text.strip(),
                "score": float(score),
                "box": box,
                "x1": min(xs),
                "y1": min(ys),
                "x2": max(xs),
                "y2": max(ys),
                "cx": (min(xs) + max(xs)) / 2.0,
                "cy": (min(ys) + max(ys)) / 2.0,
            }
        )
    return parsed


def recognize_text(image: Image.Image) -> list[tuple[str, float]]:
    return [(block["text"], block["score"]) for block in ocr_image(image) if block["text"]]

def normalize_text(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())

def text_similarity(left: str, right: str) -> float:
    return SequenceMatcher(None, normalize_text(left), normalize_text(right)).ratio()

def parse_scalar(value: str | None) -> Any:
    if value is None:
        return None
    cleaned = value.strip().replace(" ", "")
    if re.fullmatch(r"-?\d+", cleaned):
        return int(cleaned)
    if re.fullmatch(r"-?\d+\.\d+", cleaned):
        return float(cleaned)
    return value.strip()

def normalize_digit_text(text: str) -> str | None:
    cleaned = text.strip()
    if len(cleaned) == 1 and cleaned.isdigit():
        return cleaned
    digits = re.findall(r"\d", cleaned)
    if len(digits) == 1 and len(cleaned) <= 2:
        return digits[0]
    return None

def find_block(blocks: list[dict[str, Any]], needles: list[str]) -> dict[str, Any] | None:
    needle_norms = [normalize_text(needle) for needle in needles if normalize_text(needle)]
    for block in blocks:
        block_norm = normalize_text(block["text"])
        for needle_norm in needle_norms:
            if needle_norm in block_norm:
                return block

    best_block: dict[str, Any] | None = None
    best_score = 0.0
    for block in blocks:
        for needle in needles:
            score = text_similarity(block["text"], needle)
            if score > best_score:
                best_score = score
                best_block = block
    return best_block if best_score >= 0.45 else None

def nearest_value_right(blocks: list[dict[str, Any]], label_block: dict[str, Any]) -> str | None:
    candidates = []
    for block in blocks:
        if block is label_block:
            continue
        vertical_overlap = min(label_block["y2"], block["y2"]) - max(label_block["y1"], block["y1"])
        if vertical_overlap <= 0:
            continue
        if block["x1"] <= label_block["x2"]:
            continue
        candidates.append((block["x1"] - label_block["x2"], block))
    if not candidates:
        return None
    return min(candidates, key=lambda item: item[0])[1]["text"]

def group_rows(blocks: list[dict[str, Any]], start_y: float) -> list[list[dict[str, Any]]]:
    rows: list[list[dict[str, Any]]] = []
    current_row: list[dict[str, Any]] = []
    previous_cy: float | None = None
    for block in sorted((b for b in blocks if b["cy"] > start_y), key=lambda item: (item["cy"], item["x1"])):
        if previous_cy is None or block["cy"] - previous_cy <= 45:
            current_row.append(block)
        else:
            if current_row:
                rows.append(current_row)
            current_row = [block]
        previous_cy = block["cy"]
    if current_row:
        rows.append(current_row)
    return rows

def parse_document(blocks: list[dict[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {
        "document_type": None,
        "date": None,
    "batch_number": None,
    "length": None,
    "supplier": None,
        "defect_rows": [],
        "defects": {},
    }

    title_block = find_block(blocks, ["inspection record form"])
    if title_block:
        document["document_type"] = title_block["text"]

    for field_name, label in (
        ("date", ["date", "0ate"]),
        ("batch_number", ["batchnumber"]),
        ("length", ["length"]),
        ("supplier", ["supplier"]),
    ):
        label_block = find_block(blocks, label)
        if label_block:
            value_text = nearest_value_right(blocks, label_block)
            if value_text:
                document[field_name] = value_text

    defect_header = find_block(blocks, ["typeofdefects", "type of defects", "defects"])
    supplier_block = find_block(blocks, ["supplier"])
    if defect_header:
        defect_start_y = defect_header["cy"] + 30
    elif supplier_block:
        defect_start_y = supplier_block["cy"] + 70
    else:
        defect_start_y = 0

    defect_rows, defects = parse_defect_rows(
    image=Image.new("RGB", (1, 1)),
    blocks=blocks,
    start_y=defect_start_y,
    )
    document["defect_rows"] = defect_rows
    document["defects"] = defects

    return document

def convert_capture(hex_path: Path) -> dict[str, Any]:
    hex_text = read_hex_text(hex_path)
    bitmap = hex_to_bitmap(hex_text)
    ocr_ready = preprocess_for_ocr(bitmap)
    blocks = ocr_image(ocr_ready)
    document = parse_document(blocks)
    document["source_file"] = str(hex_path)
    document["ocr_block_count"] = len(blocks)
    document["image_size"] = [bitmap.width, bitmap.height]
    return document

def main() -> None:
    parser = argparse.ArgumentParser(description="Convert HMI printer HEX output into JSON.")
    parser.add_argument("input_hex", type=Path, help="Path to the HEX capture file")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON to a file instead of stdout")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    args = parser.parse_args()

    document = convert_capture(args.input_hex)
    payload = json.dumps(document, indent=args.indent, ensure_ascii=False)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)

def find_block(blocks: list[dict[str, Any]], needles: list[str]) -> dict[str, Any] | None:
    needle_norms = [normalize_text(needle) for needle in needles if normalize_text(needle)]
    for block in blocks:
        block_norm = normalize_text(block["text"])
        for needle_norm in needle_norms:
            if needle_norm in block_norm:
                return block

    best_block: dict[str, Any] | None = None
    best_score = 0.0
    for block in blocks:
        for needle in needles:
            score = text_similarity(block["text"], needle)
            if score > best_score:
                best_score = score
                best_block = block
    return best_block if best_score >= 0.45 else None


def nearest_value_right(blocks: list[dict[str, Any]], label_block: dict[str, Any]) -> str | None:
    candidates = []
    for block in blocks:
        if block is label_block:
            continue
        vertical_overlap = min(label_block["y2"], block["y2"]) - max(label_block["y1"], block["y1"])
        if vertical_overlap <= 0:
            continue
        if block["x1"] <= label_block["x2"]:
            continue
        candidates.append((block["x1"] - label_block["x2"], block))
    if not candidates:
        return None
    return min(candidates, key=lambda item: item[0])[1]["text"]


def group_rows(blocks: list[dict[str, Any]], start_y: float) -> list[list[dict[str, Any]]]:
    rows: list[list[dict[str, Any]]] = []
    current_row: list[dict[str, Any]] = []
    previous_cy: float | None = None
    for block in sorted((b for b in blocks if b["cy"] > start_y), key=lambda item: (item["cy"], item["x1"])):
        if previous_cy is None or block["cy"] - previous_cy <= 45:
            current_row.append(block)
        else:
            if current_row:
                rows.append(current_row)
            current_row = [block]
        previous_cy = block["cy"]
    if current_row:
        rows.append(current_row)
    return rows


def is_digit_block(block: dict[str, Any]) -> bool:
    return normalize_digit_text(block["text"]) is not None


def digit_blocks_in_region(
    blocks: list[dict[str, Any]],
    region: tuple[int, int, int, int],
) -> list[dict[str, Any]]:
    left, top, right, bottom = region
    return [
        block
        for block in blocks
        if is_digit_block(block)
        and block["x2"] > left
        and block["x1"] < right
        and block["y2"] > top
        and block["y1"] < bottom
    ]


def scan_digit_region(
    image: Image.Image,
    region: tuple[int, int, int, int],
) -> str | None:
    left, top, right, bottom = region
    if right <= left or bottom <= top:
        return None

    region_width = right - left
    region_height = bottom - top
    window_width = min(140, max(40, region_width))
    window_height = min(80, max(30, region_height))
    step_x = max(8, window_width // 4)
    step_y = max(4, window_height // 4)

    counts: dict[str, int] = {}
    score_totals: dict[str, float] = {}

    max_x = max(left, right - window_width)
    max_y = max(top, bottom - window_height)

    for y_pos in range(top, max_y + 1, step_y):
        for x_pos in range(left, max_x + 1, step_x):
            crop = image.crop((x_pos, y_pos, x_pos + window_width, y_pos + window_height))
            for text, score in recognize_text(crop):
                digit = normalize_digit_text(text)
                if digit is None:
                    continue
                counts[digit] = counts.get(digit, 0) + 1
                score_totals[digit] = score_totals.get(digit, 0.0) + score

    if not counts:
        return None

    return max(counts, key=lambda digit: (counts[digit], score_totals.get(digit, 0.0)))


def parse_defect_rows(
    image: Image.Image,
    blocks: list[dict[str, Any]],
    start_y: float,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    defect_rows: list[dict[str, Any]] = []
    defects: dict[str, Any] = {}

    for row in group_rows(blocks, start_y):
        ordered = sorted(row, key=lambda item: (item["x1"], item["y1"]))
        labels = [block for block in ordered if not is_digit_block(block)]
        digits = [block for block in ordered if is_digit_block(block)]

        if len(labels) < 2 or not digits:
            continue

        left_label_block = min(labels, key=lambda block: block["x1"])
        right_label_block = max(labels, key=lambda block: block["x1"])
        row_top = int(min(block["y1"] for block in ordered))
        row_bottom = int(max(block["y2"] for block in ordered))
        row_right = int(max(block["x2"] for block in ordered))

        left_region = (
            int(left_label_block["x2"]) + 4,
            row_top,
            int(right_label_block["x1"]),
            row_bottom,
        )
        right_region = (
            int(right_label_block["x2"]) + 4,
            row_top,
            max(row_right, int(right_label_block["x2"]) + 240),
            row_bottom,
        )

        left_region_digits = digit_blocks_in_region(blocks, left_region)
        right_region_digits = digit_blocks_in_region(blocks, right_region)

        left_value_text = left_region_digits[0]["text"] if left_region_digits else None
        if left_value_text is None:
            left_value_text = scan_digit_region(image, left_region)

        right_value_text = right_region_digits[-1]["text"] if right_region_digits else None
        if right_value_text is None:
            right_value_text = scan_digit_region(image, right_region)

        digit_candidates = sorted(digits, key=lambda block: (block["x1"], block["y1"]))
        if left_value_text is None and digit_candidates:
            left_value_text = normalize_digit_text(digit_candidates[0]["text"])
        if right_value_text is None and len(digit_candidates) > 1:
            right_value_text = normalize_digit_text(digit_candidates[-1]["text"])

        left_value = parse_scalar(left_value_text)
        right_value = parse_scalar(right_value_text)

        left_label = left_label_block["text"]
        right_label = right_label_block["text"]

        defect_rows.append(
            {
                "left_label": left_label,
                "left_value": left_value,
                "right_label": right_label,
                "right_value": right_value,
            }
        )
        defects[left_label] = left_value
        if right_value is not None:
            defects[right_label] = right_value

    return defect_rows, defects


def parse_document(image: Image.Image, blocks: list[dict[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {
        "document_type": None,
        "date": None,
        "batch_number": None,
        "length": None,
        "supplier": None,
        "defect_rows": [],
        "defects": {},
    }

    title_block = find_block(blocks, ["inspection record form"])
    if title_block:
        document["document_type"] = title_block["text"]

    for field_name, label in (
        ("date", ["date", "0ate"]),
        ("batch_number", ["batchnumber"]),
        ("length", ["length"]),
        ("supplier", ["supplier"]),
    ):
        label_block = find_block(blocks, label)
        if label_block:
            value_text = nearest_value_right(blocks, label_block)
            if value_text:
                document[field_name] = value_text

    defect_header = find_block(blocks, ["typeofdefects", "type of defects", "defects"])
    supplier_block = find_block(blocks, ["supplier"])
    if defect_header:
        defect_start_y = defect_header["cy"] + 30
    elif supplier_block:
        defect_start_y = supplier_block["cy"] + 70
    else:
        defect_start_y = 0

    defect_rows, defects = parse_defect_rows(image, blocks, defect_start_y)
    document["defect_rows"] = defect_rows
    document["defects"] = defects
    return document


def convert_capture(hex_path: Path) -> dict[str, Any]:
    hex_text = read_hex_text(hex_path)
    bitmap = hex_to_bitmap(hex_text)
    ocr_ready = preprocess_for_ocr(bitmap)
    blocks = ocr_image(ocr_ready)
    document = parse_document(ocr_ready, blocks)
    document["source_file"] = str(hex_path)
    document["ocr_block_count"] = len(blocks)
    document["image_size"] = [bitmap.width, bitmap.height]
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert HMI printer HEX output into JSON.")
    parser.add_argument("input_hex", type=Path, help="Path to the HEX capture file")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON to a file instead of stdout")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    args = parser.parse_args()

    document = convert_capture(args.input_hex)
    payload = json.dumps(document, indent=args.indent, ensure_ascii=False)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)


if __name__ == "__main__":
    main()