from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np

from hmi_hex_reverse_calibrated import (
    BATCH_BOX,
    DATE_BOX,
    DEFECT_LABELS,
    LENGTH_BOX,
    SUPPLIER_BOX,
    VALUE_BOXES,
    build_header_templates,
    build_value_templates,
    decode_defect_value,
    decode_header_field,
)


def read_pbm_binary(path: Path) -> np.ndarray:
    tokens: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "#" in line:
            line = line.split("#", 1)[0].strip()
        if line:
            tokens.extend(line.split())

    if len(tokens) < 3 or tokens[0] != "P1":
        raise ValueError("Expected an ASCII PBM file with P1 header.")

    width = int(tokens[1])
    height = int(tokens[2])
    values = tokens[3:]
    expected = width * height
    if len(values) < expected:
        raise ValueError(f"PBM data is incomplete: expected {expected} pixels, found {len(values)}.")

    pixels = np.asarray([int(value) for value in values[:expected]], dtype=np.uint8).reshape((height, width))
    return (pixels == 0).astype(np.uint8)


def convert_pbm(pbm_path: Path) -> dict[str, Any]:
    base_dir = Path(__file__).resolve().parent
    value_templates = build_value_templates(base_dir)
    header_templates = build_header_templates(base_dir)

    image_bin = read_pbm_binary(pbm_path)

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
        "source_file": str(pbm_path),
        "image_size": [int(image_bin.shape[1]), int(image_bin.shape[0])],
        "engine": "calibrated_reverse_no_ocr_pbm",
    }
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description="Calibration-based PBM to JSON extractor without OCR.")
    parser.add_argument("input_pbm", type=Path, help="Path to ASCII PBM capture")
    parser.add_argument("-o", "--output", type=Path, help="Output JSON path")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation")
    args = parser.parse_args()

    document = convert_pbm(args.input_pbm)
    payload = json.dumps(document, ensure_ascii=False, indent=args.indent)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
        print(f"JSON created: {args.output}")
    else:
        print(payload)


if __name__ == "__main__":
    main()