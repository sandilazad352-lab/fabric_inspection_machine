from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

MARKER = "0D0A1B4BF000"
ROW_BYTES = 240
BITS_PER_BYTE = 8
SCALE = 4

DATE_BOX = (340, 120, 900, 210)
BATCH_BOX = (500, 210, 760, 300)
LENGTH_BOX = (500, 300, 700, 380)
SUPPLIER_BOX = (500, 390, 700, 470)

DEFECT_LABELS = [
    ("shadb4", "missy4"),
    ("Thik4", "missl"),
    ("Knot's", "dyein4"),
    ("Creas4", "PRINT1"),
    ("spotlm", "holep"),
    ("spot4", "Joint4"),
]

VALUE_BOXES: list[tuple[tuple[int, int, int, int], tuple[int, int, int, int]]] = [
    ((300, 540, 440, 650), (740, 550, 930, 660)),
    ((300, 650, 430, 730), (800, 650, 920, 730)),
    ((300, 730, 420, 800), (800, 720, 920, 790)),
    ((280, 790, 430, 870), (780, 805, 930, 875)),
    ((320, 882, 460, 952), (700, 860, 950, 975)),
    ((280, 950, 430, 1035), (730, 940, 950, 1040)),
]

# Trusted references from verified screenshots.
CALIBRATION_TRUTH: dict[str, dict[str, Any]] = {
    "HMI_CAPTURE_20260409_173342.hex": {
        "date": "2026/04/09 17:14",
        "batch_number": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
        "defects": [5, 9, 8, 7, 6, 3, 4, 2, 9, 3, 6, 4],
    },
    "HMI_CAPTURE_20260411_164748.hex": {
        "date": "2026/04/11 16:28",
        "batch_number": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
        "defects": [7, 11, 8, 7, 6, 3, 4, 3, 9, 5, 8, 7],
    },
    "HMI_CAPTURE_20260411_175929.hex": {
        "date": "2026/04/11 17:40",
        "batch_number": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
        "defects": [10, 17, 11, 10, 8, 5, 5, 5, 12, 7, 10, 13],
    },
    "HMI_CAPTURE_20260411_180420.hex": {
        "date": "2026/04/11 17:45",
        "batch_number": "R-4#01",
        "length": "0.00",
        "supplier": "C-35",
        "defects": [12, 18, 13, 11, 10, 7, 6, 8, 13, 8, 12, 14],
    },
}


@dataclass(frozen=True)
class TemplateEntry:
    value: Any
    patch: np.ndarray


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
    return (np.asarray(scaled) < 128).astype(np.uint8)


def normalize_patch(crop_bin: np.ndarray, out_w: int = 80, out_h: int = 48) -> np.ndarray:
    col_nonzero = np.where(crop_bin.sum(axis=0) > 0)[0]
    row_nonzero = np.where(crop_bin.sum(axis=1) > 0)[0]

    if len(col_nonzero) == 0 or len(row_nonzero) == 0:
        return np.zeros((out_h, out_w), dtype=np.uint8)

    left = max(0, int(col_nonzero[0]) - 2)
    right = min(crop_bin.shape[1], int(col_nonzero[-1]) + 3)
    top = max(0, int(row_nonzero[0]) - 2)
    bottom = min(crop_bin.shape[0], int(row_nonzero[-1]) + 3)

    tight = crop_bin[top:bottom, left:right]
    pil = Image.fromarray((tight * 255).astype(np.uint8), mode="L")
    resized = pil.resize((out_w, out_h), Image.Resampling.NEAREST)
    return (np.asarray(resized) > 127).astype(np.uint8)


def patch_similarity(a: np.ndarray, b: np.ndarray) -> float:
    return 1.0 - float(np.mean(np.abs(a.astype(np.int16) - b.astype(np.int16))))


def _cell_boxes_flat() -> list[tuple[int, int, int, int]]:
    return [box for pair in VALUE_BOXES for box in pair]


def build_value_templates(base_dir: Path) -> dict[int, list[TemplateEntry]]:
    templates: dict[int, list[TemplateEntry]] = {idx: [] for idx in range(12)}

    for file_name, truth in CALIBRATION_TRUTH.items():
        hex_path = base_dir / file_name
        if not hex_path.exists():
            continue

        image_bin = preprocess_binary(hex_to_bitmap(read_hex_text(hex_path)))
        values: list[int] = truth["defects"]

        for idx, (box, value) in enumerate(zip(_cell_boxes_flat(), values)):
            left, top, right, bottom = box
            patch = normalize_patch(image_bin[top:bottom, left:right])
            templates[idx].append(TemplateEntry(value=value, patch=patch))

    return templates


def build_header_templates(base_dir: Path) -> dict[str, list[TemplateEntry]]:
    templates = {
        "date": [],
        "batch_number": [],
        "length": [],
        "supplier": [],
    }

    field_map = {
        "date": DATE_BOX,
        "batch_number": BATCH_BOX,
        "length": LENGTH_BOX,
        "supplier": SUPPLIER_BOX,
    }

    for file_name, truth in CALIBRATION_TRUTH.items():
        hex_path = base_dir / file_name
        if not hex_path.exists():
            continue

        image_bin = preprocess_binary(hex_to_bitmap(read_hex_text(hex_path)))
        for field, box in field_map.items():
            left, top, right, bottom = box
            patch = normalize_patch(image_bin[top:bottom, left:right], out_w=140, out_h=56)
            templates[field].append(TemplateEntry(value=truth[field], patch=patch))

    return templates


def decode_from_templates(
    patch: np.ndarray,
    entries: list[TemplateEntry],
    min_similarity: float,
    min_gap: float,
) -> Any | None:
    if not entries:
        return None

    scored = sorted(
        ((entry.value, patch_similarity(patch, entry.patch)) for entry in entries),
        key=lambda item: item[1],
        reverse=True,
    )

    best_value, best_score = scored[0]
    second_score = scored[1][1] if len(scored) > 1 else 0.0

    if best_score < min_similarity:
        return None
    if (best_score - second_score) < min_gap:
        return None

    return best_value


def decode_defect_value(
    image_bin: np.ndarray,
    box: tuple[int, int, int, int],
    entries: list[TemplateEntry],
) -> int | None:
    left, top, right, bottom = box
    patch = normalize_patch(image_bin[top:bottom, left:right])
    value = decode_from_templates(patch, entries, min_similarity=0.76, min_gap=0.02)
    if value is None:
        return None
    value = int(value)
    if value < 0 or value > 20:
        return None
    return value


def decode_header_field(
    image_bin: np.ndarray,
    box: tuple[int, int, int, int],
    entries: list[TemplateEntry],
) -> str | None:
    left, top, right, bottom = box
    patch = normalize_patch(image_bin[top:bottom, left:right], out_w=140, out_h=56)
    return decode_from_templates(patch, entries, min_similarity=0.78, min_gap=0.015)


def convert_capture(hex_path: Path) -> dict[str, Any]:
    base_dir = Path(__file__).resolve().parent

    value_templates = build_value_templates(base_dir)
    header_templates = build_header_templates(base_dir)

    hex_text = read_hex_text(hex_path)
    bitmap = hex_to_bitmap(hex_text)
    image_bin = preprocess_binary(bitmap)

    rows: list[dict[str, Any]] = []
    defects: dict[str, Any] = {}

    for idx, ((left_label, right_label), (left_box, right_box)) in enumerate(zip(DEFECT_LABELS, VALUE_BOXES)):
        left_value = decode_defect_value(image_bin, left_box, value_templates[idx * 2])
        right_value = decode_defect_value(image_bin, right_box, value_templates[(idx * 2) + 1])

        rows.append(
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

    document = {
        "document_type": "Inspection Record Form",
        "date": decode_header_field(image_bin, DATE_BOX, header_templates["date"]),
        "batch_number": decode_header_field(image_bin, BATCH_BOX, header_templates["batch_number"]),
        "length": decode_header_field(image_bin, LENGTH_BOX, header_templates["length"]),
        "supplier": decode_header_field(image_bin, SUPPLIER_BOX, header_templates["supplier"]),
        "defect_rows": rows,
        "defects": defects,
        "source_file": str(hex_path),
        "image_size": [bitmap.width, bitmap.height],
        "engine": "calibrated_reverse_no_ocr_cell_matching",
    }
    return document


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Calibration-based reverse parser for HMI HEX without OCR library."
    )
    parser.add_argument("input_hex", type=Path, help="Path to HEX capture")
    parser.add_argument("-o", "--output", type=Path, help="Output JSON path")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation")
    args = parser.parse_args()

    document = convert_capture(args.input_hex)
    payload = json.dumps(document, ensure_ascii=False, indent=args.indent)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
        print(f"JSON created: {args.output}")
    else:
        print(payload)


if __name__ == "__main__":
    main()
