from __future__ import annotations

import argparse
import json
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

MARKER = "0D0A1B4BF000"
ROW_BYTES = 240
BITS_PER_BYTE = 8
SCALE = 4

# Value cells on the scaled bitmap (960 x 1280)
VALUE_BOXES: list[tuple[tuple[int, int, int, int], tuple[int, int, int, int]]] = [
    ((300, 540, 440, 650), (740, 550, 930, 660)),
    ((300, 650, 430, 730), (800, 650, 920, 730)),
    ((300, 730, 420, 800), (800, 720, 920, 790)),
    ((280, 790, 430, 870), (780, 805, 930, 875)),
    ((320, 882, 460, 952), (700, 860, 950, 975)),
    ((280, 950, 430, 1035), (730, 940, 950, 1040)),
]

DEFECT_LABELS = [
    ("shadb4", "missy4"),
    ("Thik4", "missl"),
    ("Knot's", "dyein4"),
    ("Creas4", "PRINT1"),
    ("spotlm", "holep"),
    ("spot4", "Joint4"),
]

DATE_BOX = (340, 120, 900, 210)
BATCH_BOX = (500, 210, 760, 300)
LENGTH_BOX = (500, 300, 700, 380)
SUPPLIER_BOX = (500, 390, 700, 470)

# Known-good references used to learn glyph templates (no OCR).
REFERENCE_VALUES: dict[str, dict[str, Any]] = {
    "HMI_CAPTURE_20260411_164748.hex": {
        "defects": ["7", "11", "8", "7", "6", "3", "4", "3", "9", "5", "8", "7"],
        "date": "2026/04/11 16:28",
        "batch": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
    },
    "HMI_CAPTURE_20260411_180420.hex": {
        "defects": ["12", "18", "13", "11", "10", "7", "6", "8", "13", "8", "12", "14"],
        "date": "2026/04/11 17:45",
        "batch": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
    },
}


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


def preprocess_binary(image: Image.Image, scale: int = SCALE) -> np.ndarray:
    scaled = image.convert("L").resize((image.width * scale, image.height * scale), Image.Resampling.NEAREST)
    array = np.asarray(scaled)
    # Ink pixels are dark; represent ink as 1.
    return (array < 128).astype(np.uint8)


def _split_runs(mask: np.ndarray, min_width: int = 3) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    start: int | None = None
    for idx, value in enumerate(mask.tolist()):
        if value and start is None:
            start = idx
        elif not value and start is not None:
            if idx - start >= min_width:
                runs.append((start, idx))
            start = None
    if start is not None and len(mask) - start >= min_width:
        runs.append((start, len(mask)))
    return runs


def _extract_components(crop_bin: np.ndarray) -> list[np.ndarray]:
    col_sum = crop_bin.sum(axis=0)
    runs = _split_runs(col_sum > 0, min_width=3)

    refined: list[tuple[int, int]] = []
    for left, right in runs:
        width = right - left
        if width > 52:
            mid = left + width // 2
            valley = left + int(np.argmin(col_sum[left:right]))
            split = valley if left + 8 < valley < right - 8 else mid
            refined.append((left, split))
            refined.append((split, right))
        else:
            refined.append((left, right))

    components: list[np.ndarray] = []
    for left, right in refined:
        comp = crop_bin[:, left:right]
        row_sum = comp.sum(axis=1)
        row_runs = _split_runs(row_sum > 0, min_width=3)
        if not row_runs:
            continue
        top, bottom = row_runs[0][0], row_runs[-1][1]
        glyph = comp[top:bottom, :]
        if glyph.shape[0] < 6 or glyph.shape[1] < 4:
            continue
        components.append(glyph)
    return components


def _normalize_component(component: np.ndarray, out_w: int = 24, out_h: int = 36) -> np.ndarray:
    pil = Image.fromarray((component * 255).astype(np.uint8), mode="L")
    resized = pil.resize((out_w, out_h), Image.Resampling.NEAREST)
    return (np.asarray(resized) > 127).astype(np.uint8)


def _digit_similarity(a: np.ndarray, b: np.ndarray) -> float:
    # 1.0 = identical, 0.0 = fully different
    return 1.0 - float(np.mean(np.abs(a.astype(np.int16) - b.astype(np.int16))))


@lru_cache(maxsize=1)
def build_digit_templates() -> dict[str, list[np.ndarray]]:
    templates: dict[str, list[np.ndarray]] = {str(d): [] for d in range(10)}
    base_dir = Path(__file__).resolve().parent

    for file_name, payload in REFERENCE_VALUES.items():
        hex_path = base_dir / file_name
        if not hex_path.exists():
            continue

        image_bin = preprocess_binary(hex_to_bitmap(read_hex_text(hex_path)))
        cell_values: list[str] = payload["defects"]
        boxes = [box for pair in VALUE_BOXES for box in pair]

        for box, expected in zip(boxes, cell_values):
            left, top, right, bottom = box
            crop = image_bin[top:bottom, left:right]
            components = _extract_components(crop)
            if len(components) != len(expected):
                continue

            for component, ch in zip(components, expected):
                if ch.isdigit():
                    templates[ch].append(_normalize_component(component))

        # Add templates from header numeric fields.
        for box, expected in (
            (DATE_BOX, payload["date"]),
            (BATCH_BOX, payload["batch"]),
            (LENGTH_BOX, payload["length"]),
            (SUPPLIER_BOX, payload["supplier"]),
        ):
            left, top, right, bottom = box
            crop = image_bin[top:bottom, left:right]
            components = _extract_components(crop)
            expected_digits = [ch for ch in expected if ch.isdigit()]
            if len(components) != len(expected_digits):
                continue
            for component, ch in zip(components, expected_digits):
                templates[ch].append(_normalize_component(component))

    return templates


def read_digits_from_box(image_bin: np.ndarray, box: tuple[int, int, int, int], min_score: float = 0.62) -> str | None:
    templates = build_digit_templates()
    left, top, right, bottom = box
    crop = image_bin[top:bottom, left:right]
    components = _extract_components(crop)
    if not components:
        return None
    if len(components) > 2:
        return None

    decoded: list[str] = []
    for component in components:
        candidate = _normalize_component(component)
        score_by_digit: dict[str, float] = {}

        for digit, refs in templates.items():
            digit_best = -1.0
            for ref in refs:
                score = _digit_similarity(candidate, ref)
                if score > digit_best:
                    digit_best = score
            if digit_best >= 0:
                score_by_digit[digit] = digit_best

        if not score_by_digit:
            return None

        ranked = sorted(score_by_digit.items(), key=lambda item: item[1], reverse=True)
        best_digit, best_score = ranked[0]
        second_score = ranked[1][1] if len(ranked) > 1 else 0.0

        if best_score < min_score:
            return None
        if (best_score - second_score) < 0.08:
            return None
        decoded.append(best_digit)

    if len(decoded) == 2 and decoded[0] == "1" and decoded[1] == "1":
        # This pair is frequently produced by ambiguous shapes in these captures.
        return None

    return "".join(decoded)


def parse_date(image_bin: np.ndarray) -> str | None:
    raw = read_digits_from_box(image_bin, DATE_BOX, min_score=0.60)
    if raw is None or len(raw) < 12:
        return None
    raw = raw[:12]
    return f"{raw[0:4]}/{raw[4:6]}/{raw[6:8]} {raw[8:10]}:{raw[10:12]}"


def parse_batch(image_bin: np.ndarray) -> str | None:
    raw = read_digits_from_box(image_bin, BATCH_BOX, min_score=0.60)
    if raw is None or len(raw) < 3:
        return None
    raw = raw[:3]
    return f"R-{raw[0]}#{raw[1:3]}"


def parse_length(image_bin: np.ndarray) -> str | None:
    raw = read_digits_from_box(image_bin, LENGTH_BOX, min_score=0.60)
    if raw is None:
        return None
    if len(raw) >= 3:
        return f"{raw[0]}.{raw[1:3]}"
    if len(raw) == 2:
        return f"{raw[0]}.{raw[1]}0"
    return raw


def parse_supplier(image_bin: np.ndarray) -> str | None:
    raw = read_digits_from_box(image_bin, SUPPLIER_BOX, min_score=0.60)
    if raw is None or len(raw) < 2:
        return None
    return f"C-{raw[:2]}"


def parse_defects(image_bin: np.ndarray) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    defects: dict[str, Any] = {}

    for (left_label, right_label), (left_box, right_box) in zip(DEFECT_LABELS, VALUE_BOXES):
        left_raw = read_digits_from_box(image_bin, left_box)
        right_raw = read_digits_from_box(image_bin, right_box)

        left_value = int(left_raw) if left_raw is not None else None
        right_value = int(right_raw) if right_raw is not None else None

        row = {
            "left_label": left_label,
            "left_value": left_value,
            "right_label": right_label,
            "right_value": right_value,
        }
        rows.append(row)

        defects[left_label] = left_value
        if right_value is not None:
            defects[right_label] = right_value

    return rows, defects


def convert_capture_no_ocr(hex_path: Path) -> dict[str, Any]:
    hex_text = read_hex_text(hex_path)
    bitmap = hex_to_bitmap(hex_text)
    image_bin = preprocess_binary(bitmap)

    defect_rows, defects = parse_defects(image_bin)

    return {
        "document_type": "Inspection Record Form",
        "date": parse_date(image_bin),
        "batch_number": parse_batch(image_bin),
        "length": parse_length(image_bin),
        "supplier": parse_supplier(image_bin),
        "defect_rows": defect_rows,
        "defects": defects,
        "source_file": str(hex_path),
        "image_size": [bitmap.width, bitmap.height],
        "engine": "no_ocr_template_matching",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert HMI HEX to JSON without OCR (template matching).")
    parser.add_argument("input_hex", type=Path, help="Path to HEX capture")
    parser.add_argument("-o", "--output", type=Path, help="Output JSON path")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation")
    args = parser.parse_args()

    payload = convert_capture_no_ocr(args.input_hex)
    text = json.dumps(payload, ensure_ascii=False, indent=args.indent)

    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"JSON created: {args.output}")
    else:
        print(text)


if __name__ == "__main__":
    main()
