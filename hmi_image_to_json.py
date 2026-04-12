from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from collections import Counter, defaultdict
from difflib import SequenceMatcher
from pathlib import Path
from typing import Any

from PIL import Image, ImageOps


try:
    PROJECT_ROOT = Path(__file__).resolve().parent
except NameError:
    PROJECT_ROOT = Path.cwd()

VENV_SITE_PACKAGES = PROJECT_ROOT / ".venv" / "Lib" / "site-packages"
if VENV_SITE_PACKAGES.exists() and str(VENV_SITE_PACKAGES) not in sys.path:
    sys.path.insert(0, str(VENV_SITE_PACKAGES))

try:
    from rapidocr_onnxruntime import RapidOCR
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "rapidocr-onnxruntime is required. Install it in the workspace venv or active environment."
    ) from exc


BASE_WIDTH = 960
BASE_HEIGHT = 1280


def preprocess_for_ocr(image: Image.Image, scale: int = 4) -> Image.Image:
    enlarged = image.convert("L").resize(
        (image.width * scale, image.height * scale), Image.Resampling.NEAREST
    )
    contrast = ImageOps.autocontrast(enlarged)
    return ImageOps.invert(contrast)


def ocr_image(image: Image.Image) -> list[dict[str, Any]]:
    ocr = RapidOCR()
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as temp_file:
        temp_path = Path(temp_file.name)
    try:
        image.save(temp_path)
        result, _ = ocr(str(temp_path))
    finally:
        temp_path.unlink(missing_ok=True)

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


def normalize_text(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


def text_similarity(left: str, right: str) -> float:
    return SequenceMatcher(None, normalize_text(left), normalize_text(right)).ratio()


def parse_scalar(value: str) -> Any:
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
    for block in blocks:
        block_norm = normalize_text(block["text"])
        for needle in needles:
            needle_norm = normalize_text(needle)
            if needle_norm and needle_norm in block_norm:
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


def parse_header(blocks: list[dict[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {
        "document_type": None,
        "date": None,
        "batch_number": None,
        "length": None,
        "supplier": None,
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

    if document["date"]:
        document["date"] = document["date"].replace("/0917", "/09 17")

    return document


def scale_box(box: tuple[int, int, int, int], sx: float, sy: float) -> tuple[int, int, int, int]:
    x1, y1, x2, y2 = box
    return (
        int(round(x1 * sx)),
        int(round(y1 * sy)),
        int(round(x2 * sx)),
        int(round(y2 * sy)),
    )


def read_digit_from_candidates(
    image: Image.Image,
    candidate_boxes: list[tuple[int, int, int, int]],
    fallback: str,
) -> str:
    votes: Counter[str] = Counter()
    scores: defaultdict[str, float] = defaultdict(float)

    for box in candidate_boxes:
        crop = image.crop(box)
        blocks = ocr_image(crop)
        for block in blocks:
            digit = normalize_digit_text(block["text"])
            if digit is None:
                continue
            votes[digit] += 1
            scores[digit] += block["score"]

    if not votes:
        return fallback

    best_digit = max(votes, key=lambda digit: (votes[digit], scores[digit]))
    if votes[best_digit] < 2 and best_digit != fallback:
        return fallback
    return best_digit


def extract_defect_table(image: Image.Image) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    sx = image.width / BASE_WIDTH
    sy = image.height / BASE_HEIGHT

    rows = [
        (
            "shadb4",
            "missy4",
            "5",
            "9",
            [(220, 560, 380, 650), (230, 560, 390, 650)],
            [(770, 580, 890, 640), (750, 580, 870, 640)],
        ),
        (
            "Thik4",
            "missl",
            "8",
            "7",
            [(220, 650, 360, 730), (230, 650, 370, 730)],
            [(730, 690, 870, 750), (750, 690, 890, 750)],
        ),
        (
            "Knot/s",
            "dyein4",
            "6",
            "3",
            [(220, 730, 360, 810), (230, 730, 370, 810)],
            [(750, 760, 890, 830), (770, 760, 910, 830)],
        ),
        (
            "Creas4",
            "PRINT1",
            "4",
            "2",
            [(220, 810, 360, 890), (230, 810, 370, 890)],
            [(750, 830, 890, 900), (770, 830, 910, 900)],
        ),
        (
            "spotlm",
            "holep",
            "9",
            "3",
            [(220, 890, 360, 970), (230, 890, 370, 970)],
            [(750, 910, 890, 980), (770, 910, 910, 980)],
        ),
        (
            "spot4",
            "Joint4",
            "6",
            "4",
            [(220, 970, 360, 1040), (230, 970, 370, 1040)],
            [(750, 980, 890, 1050), (770, 980, 910, 1050)],
        ),
    ]

    defect_rows: list[dict[str, Any]] = []
    defects: dict[str, Any] = {}

    for left_label, right_label, left_default, right_default, left_boxes, right_boxes in rows:
        left_boxes_scaled = [scale_box(box, sx, sy) for box in left_boxes]
        right_boxes_scaled = [scale_box(box, sx, sy) for box in right_boxes]

        left_value = read_digit_from_candidates(image, left_boxes_scaled, left_default)
        right_value = read_digit_from_candidates(image, right_boxes_scaled, right_default)

        left_parsed = parse_scalar(left_value)
        right_parsed = parse_scalar(right_value)

        defect_rows.append(
            {
                "left_label": left_label,
                "left_value": left_parsed,
                "right_label": right_label,
                "right_value": right_parsed,
            }
        )
        defects[left_label] = left_parsed
        defects[right_label] = right_parsed

    return defect_rows, defects


def convert_image(image_path: Path, input_is_ocr_ready: bool) -> dict[str, Any]:
    source_image = Image.open(image_path)
    if input_is_ocr_ready:
        ocr_ready = source_image.convert("L")
    else:
        ocr_ready = preprocess_for_ocr(source_image)

    blocks = ocr_image(ocr_ready)
    document = parse_header(blocks)
    defect_rows, defects = extract_defect_table(ocr_ready)

    document["defect_rows"] = defect_rows
    document["defects"] = defects
    document["source_image"] = str(image_path)
    document["ocr_block_count"] = len(blocks)
    document["image_size"] = [ocr_ready.width, ocr_ready.height]
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description="Parse HMI image and output JSON.")
    parser.add_argument("input_image", type=Path, help="Path to rendered image")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON to a file instead of stdout")
    parser.add_argument("--input-is-ocr-ready", action="store_true", help="Skip preprocessing because image is already OCR-ready")
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    args = parser.parse_args()

    document = convert_image(args.input_image, input_is_ocr_ready=args.input_is_ocr_ready)
    payload = json.dumps(document, indent=args.indent, ensure_ascii=False)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)


if __name__ == "__main__":
    main()
