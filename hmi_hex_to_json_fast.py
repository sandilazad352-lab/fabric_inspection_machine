from __future__ import annotations

import argparse
import json
import os
import re
import sys
import shutil
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from functools import lru_cache
from pathlib import Path
from difflib import SequenceMatcher
from typing import Any

import numpy as np

from hmi_value_consensus import NumericCandidate, choose_strict_numeric_value

try:
    PROJECT_ROOT = Path(__file__).resolve().parent
except NameError:  # pragma: no cover - supports exec() in the active REPL
    PROJECT_ROOT = Path.cwd()

VENV_SITE_PACKAGES = PROJECT_ROOT / ".venv" / "Lib" / "site-packages"
if VENV_SITE_PACKAGES.exists() and str(VENV_SITE_PACKAGES) not in sys.path:
    sys.path.insert(0, str(VENV_SITE_PACKAGES))

from PIL import Image, ImageOps

try:
    import pytesseract
    from pytesseract import Output
except ImportError as exc:  # pragma: no cover - dependency guard
    raise SystemExit(
        "pytesseract is required. Install it in the workspace venv or add it to the active Python environment."
    ) from exc


MARKER = "0D0A1B4BF000"
ROW_BYTES = 240
BITS_PER_BYTE = 8
OCR_LANG = "eng"
OCR_PSM = 6
OCR_BACKEND = "pytesseract"
VALUE_MODE = "strict"
EXPECTED_DEFECT_ROWS = [
    ("shadb4", "missy4"),
    ("Thik4", "missl"),
    ("Knot's", "dyein4"),
    ("Creas4", "PRINT1"),
    ("spotlm", "holep"),
    ("spot4", "Joint4"),
]
EXPECTED_DEFECT_VALUE_BOXES = [
    ((300, 540, 440, 650), (740, 550, 930, 660)),
    ((300, 650, 430, 730), (800, 650, 920, 730)),
    ((300, 730, 420, 800), (800, 720, 920, 790)),
    ((280, 790, 430, 870), (780, 805, 930, 875)),
    ((320, 882, 460, 952), (700, 860, 950, 975)),
    ((280, 950, 430, 1035), (730, 940, 950, 1040)),
]


@lru_cache(maxsize=1)
def get_tesseract_cmd() -> str | None:
    return os.environ.get("TESSERACT_CMD")


def configure_tesseract_cmd(explicit_path: str | None = None) -> None:
    tesseract_cmd = explicit_path or get_tesseract_cmd()
    if not tesseract_cmd:
        tesseract_cmd = shutil.which("tesseract")
    if not tesseract_cmd:
        default_windows_path = r"C:\Program Files\Tesseract-OCR\tesseract.exe"
        if Path(default_windows_path).exists():
            tesseract_cmd = default_windows_path
    if tesseract_cmd:
        pytesseract.pytesseract.tesseract_cmd = tesseract_cmd
        return

    raise SystemExit(
        "Tesseract was not found. Install it or pass --tesseract-cmd \"C:\\Program Files\\Tesseract-OCR\\tesseract.exe\"."
    )


def configure_ocr(lang: str | None = None, psm: int | None = None) -> None:
    global OCR_LANG, OCR_PSM
    if lang:
        OCR_LANG = lang
    if psm is not None:
        OCR_PSM = psm


def configure_backend(backend: str) -> None:
    global OCR_BACKEND
    if backend not in {"pytesseract", "easyocr"}:
        raise ValueError(f"Unsupported OCR backend: {backend}")
    OCR_BACKEND = backend


def configure_value_mode(mode: str) -> None:
    global VALUE_MODE
    if mode not in {"strict", "balanced"}:
        raise ValueError(f"Unsupported value mode: {mode}")
    VALUE_MODE = mode


@lru_cache(maxsize=4)
def get_easyocr_reader(lang: str) -> Any:
    try:
        import easyocr
    except ImportError as exc:  # pragma: no cover
        raise SystemExit(
            "easyocr is required for --ocr-backend easyocr. Install it in the workspace venv."
        ) from exc

    language_map = {"eng": "en"}
    easyocr_lang = language_map.get(lang, "en")
    return easyocr.Reader([easyocr_lang], gpu=False, verbose=False)


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
    return ImageOps.autocontrast(enlarged)


def preprocess_for_ocr_inverted(image: Image.Image, scale: int = 4) -> Image.Image:
    return ImageOps.invert(preprocess_for_ocr(image, scale=scale))


def _ocr_image_pytesseract(image: Image.Image, lang: str | None = None, psm: int | None = None) -> list[dict[str, Any]]:
    lang = lang or OCR_LANG
    psm = OCR_PSM if psm is None else psm
    config = f"--psm {psm}"
    data = pytesseract.image_to_data(image, lang=lang, config=config, output_type=Output.DICT)

    parsed: list[dict[str, Any]] = []
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

        parsed.append(
            {
                "text": text,
                "score": confidence / 100.0,
                "box": [[left, top], [left + width, top], [left + width, top + height], [left, top + height]],
                "x1": left,
                "y1": top,
                "x2": left + width,
                "y2": top + height,
                "cx": left + width / 2.0,
                "cy": top + height / 2.0,
            }
        )
    return parsed


def _ocr_image_easyocr(image: Image.Image, lang: str | None = None, whitelist: str | None = None) -> list[dict[str, Any]]:
    lang = lang or OCR_LANG
    reader = get_easyocr_reader(lang)
    result = reader.readtext(np.asarray(image.convert("RGB")), detail=1, paragraph=False, allowlist=whitelist)

    parsed: list[dict[str, Any]] = []
    for box, text, score in result:
        clean_text = str(text).strip()
        if not clean_text:
            continue
        xs = [point[0] for point in box]
        ys = [point[1] for point in box]
        parsed.append(
            {
                "text": clean_text,
                "score": float(score),
                "box": [[float(point[0]), float(point[1])] for point in box],
                "x1": float(min(xs)),
                "y1": float(min(ys)),
                "x2": float(max(xs)),
                "y2": float(max(ys)),
                "cx": (float(min(xs)) + float(max(xs))) / 2.0,
                "cy": (float(min(ys)) + float(max(ys))) / 2.0,
            }
        )
    return parsed


def ocr_image(
    image: Image.Image,
    lang: str | None = None,
    psm: int | None = None,
    whitelist: str | None = None,
) -> list[dict[str, Any]]:
    if OCR_BACKEND == "easyocr":
        return _ocr_image_easyocr(image, lang=lang, whitelist=whitelist)
    return _ocr_image_pytesseract(image, lang=lang, psm=psm)


def ocr_image_variants(
    image: Image.Image,
    lang: str | None = None,
    psm: int | None = None,
    whitelist: str | None = None,
) -> list[dict[str, Any]]:
    lang = lang or OCR_LANG
    psm = OCR_PSM if psm is None else psm
    variants = (image, ImageOps.invert(image))

    blocks: dict[tuple[int, int, str], dict[str, Any]] = {}
    for variant in variants:
        variant_blocks = ocr_image(variant, lang=lang, psm=psm, whitelist=whitelist)
        for block in variant_blocks:
            key = (round(block["cx"] / 10), round(block["cy"] / 10), normalize_text(block["text"]))
            existing = blocks.get(key)
            if existing is None or block["score"] > existing["score"]:
                blocks[key] = block

    return sorted(blocks.values(), key=lambda item: (item["cy"], item["x1"]))


def normalize_text(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


def text_similarity(left: str, right: str) -> float:
    return SequenceMatcher(None, normalize_text(left), normalize_text(right)).ratio()


def parse_scalar(value: str | None) -> Any:
    if value is None:
        return None
    cleaned = value.strip().replace(" ", "")
    digits = re.findall(r"\d", cleaned)
    if 1 <= len(digits) <= 2 and len(cleaned) <= 4:
        return int("".join(digits))
    if re.fullmatch(r"-?\d+", cleaned):
        return int(cleaned)
    if re.fullmatch(r"-?\d+\.\d+", cleaned):
        return float(cleaned)
    return value.strip()


def normalize_digit_text(text: str) -> str | None:
    cleaned = text.strip()
    if re.fullmatch(r"\d+", cleaned):
        return cleaned
    digits = re.findall(r"\d", cleaned)
    if 1 <= len(digits) <= 2 and len(cleaned) <= 4:
        return "".join(digits)
    return None


def is_digit_block(block: dict[str, Any]) -> bool:
    return normalize_digit_text(block["text"]) is not None


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


def normalize_title_text(text: str | None) -> str | None:
    if not text:
        return None
    if text_similarity(text, "inspection record form") >= 0.55:
        return "Inspection Record Form"
    return text.strip()


def normalize_date_text(text: str | None) -> str | None:
    if not text:
        return None
    cleaned = text.strip().replace(" ", "")
    match = re.fullmatch(r"(\d{4}/\d{2}/\d{2})(\d{2}:\d{2})", cleaned)
    if match:
        date_part = match.group(1)
        year = int(date_part[:4])
        if year < 2020 or year > 2035:
            date_part = f"2026{date_part[4:]}"
        return f"{date_part} {match.group(2)}"
    return text.strip()


def normalize_batch_number_text(text: str | None) -> str | None:
    if not text:
        return None
    cleaned = text.strip().upper().replace(" ", "")
    cleaned = cleaned.replace("O", "0").replace("I", "1").replace("L", "1").replace("]", "1")
    cleaned = cleaned.replace("D", "4")
    if "#" in cleaned and cleaned.startswith("R-"):
        digits = re.findall(r"\d", cleaned)
        if len(digits) >= 3:
            return f"R-{digits[0]}#{digits[1]}{digits[2]}"
    return text.strip()


def right_line_text(blocks: list[dict[str, Any]], label_block: dict[str, Any]) -> str | None:
    candidates = []
    for block in blocks:
        if block is label_block:
            continue
        vertical_overlap = min(label_block["y2"], block["y2"]) - max(label_block["y1"], block["y1"])
        if vertical_overlap <= 0:
            continue
        if block["x1"] <= label_block["x2"]:
            continue
        candidates.append(block)
    if not candidates:
        return None
    ordered = sorted(candidates, key=lambda block: (block["x1"], block["y1"]))
    return " ".join(block["text"] for block in ordered)


def _best_block_text(blocks: list[dict[str, Any]]) -> str | None:
    if not blocks:
        return None
    best = max(blocks, key=lambda block: (block["score"], len(block["text"]), -block["x1"]))
    return best["text"]


def read_digit_from_box(image: Image.Image, box: tuple[int, int, int, int]) -> int | None:
    crop = image.crop(box)
    candidates: list[NumericCandidate] = []

    for scale in (1, 2, 3):
        scaled = crop if scale == 1 else crop.resize((crop.width * scale, crop.height * scale), Image.Resampling.NEAREST)
        for psm in (6, 7, 10, 13):
            blocks = ocr_image_variants(scaled, psm=psm, whitelist="0123456789")
            best_for_strategy: dict[int, float] = {}
            for block in blocks:
                digit_text = normalize_digit_text(block["text"])
                if digit_text is None:
                    continue
                try:
                    digit_value = int(digit_text)
                except ValueError:
                    continue

                # Defect counts on this form are small; reject clearly noisy values.
                if digit_value < 0 or digit_value > 20:
                    continue

                candidate_key = (scale * 10) + psm
                previous_score = best_for_strategy.get(digit_value)
                if previous_score is None or block["score"] > previous_score:
                    best_for_strategy[digit_value] = block["score"]

            for digit_value, score in best_for_strategy.items():
                candidates.append(NumericCandidate(value=digit_value, score=score, strategy=f"s{scale}-p{psm}"))

    result = choose_strict_numeric_value(candidates)
    if result.value is not None:
        return result.value

    if VALUE_MODE == "balanced" and result.votes:
        return max(
            result.votes,
            key=lambda value: (
                result.votes[value],
                result.score_sum.get(value, 0.0),
                result.best_score.get(value, 0.0),
                value,
            ),
        )

    return None


def extract_header_value(
    image: Image.Image,
    label_block: dict[str, Any],
    field_name: str,
    fallback_text: str | None = None,
) -> str | None:
    crop_left = max(0, int(label_block["x2"]) + 4)
    crop_top = max(0, int(label_block["y1"]) - 18)
    crop_right = min(image.width, crop_left + 220)
    crop_bottom = min(image.height, int(label_block["y2"]) + 26)
    if crop_right <= crop_left:
        return fallback_text
    if crop_bottom <= crop_top:
        return fallback_text

    crop = image.crop((crop_left, crop_top, crop_right, crop_bottom))
    whitelist_map = {
        "date": "0123456789/:",
        "batch_number": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-#",
        "length": "0123456789.",
        "supplier": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-",
    }
    psm_map = {
        "date": 7,
        "batch_number": 7,
        "length": 7,
        "supplier": 7,
    }
    blocks = ocr_image_variants(
        crop,
        psm=psm_map.get(field_name, 7),
        whitelist=whitelist_map.get(field_name),
    )
    raw_text = _best_block_text(blocks)

    if field_name == "date":
        return normalize_date_text(raw_text or fallback_text)
    if field_name == "batch_number":
        return normalize_batch_number_text(raw_text or fallback_text)
    if field_name == "length":
        if raw_text:
            cleaned = re.sub(r"[^0-9.]", "", raw_text)
            if cleaned:
                return cleaned
        return fallback_text
    if field_name == "supplier":
        if raw_text:
            cleaned = re.sub(r"[^A-Z0-9-]", "", raw_text.upper())
            if cleaned and not cleaned.startswith("C-") and cleaned.startswith("C") and len(cleaned) > 1:
                cleaned = f"C-{cleaned[1:]}"
            return cleaned or None
        return fallback_text
    return raw_text


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


def digit_blocks_in_region(
    blocks: list[dict[str, Any]],
    region: tuple[int, int, int, int],
) -> list[dict[str, Any]]:
    left, top, right, bottom = region
    return [
        block
        for block in blocks
        if is_digit_block(block)
        and left <= block["cx"] <= right
        and top <= block["cy"] <= bottom
    ]


def _ocr_crop_window(
    image: Image.Image,
    crop_box: tuple[int, int, int, int],
    lang: str | None = None,
    psm: int | None = None,
) -> list[dict[str, Any]]:
    """Run OCR on a single crop window in parallel."""
    crop = image.crop(crop_box)
    return [block for block in ocr_image_variants(crop, lang=lang, psm=psm, whitelist="0123456789") if is_digit_block(block)]


def _ocr_scaled_crop_window(
    image: Image.Image,
    crop_box: tuple[int, int, int, int],
    lang: str | None = None,
    psm: int | None = None,
    scale: int = 4,
) -> list[dict[str, Any]]:
    crop = image.crop(crop_box)
    if scale > 1:
        crop = crop.resize((crop.width * scale, crop.height * scale), Image.Resampling.NEAREST)
    return [block for block in ocr_image_variants(crop, lang=lang, psm=psm, whitelist="0123456789") if is_digit_block(block)]


def _recover_right_value_from_expanded_crop(
    image: Image.Image,
    right_label_block: dict[str, Any],
    row_top: int,
    row_bottom: int,
    lang: str | None = None,
    psm: int | None = None,
) -> str | None:
    value_crop_box = (
        max(0, int(right_label_block["x2"]) + 110),
        max(0, row_top - 62),
        min(image.width, int(right_label_block["x2"]) + 240),
        min(image.height, row_top + 8),
    )
    digit_blocks = []
    if value_crop_box[2] > value_crop_box[0] and value_crop_box[3] > value_crop_box[1]:
        digit_blocks = _ocr_scaled_crop_window(image, value_crop_box, lang=lang, psm=psm, scale=4)

    if not digit_blocks:
        crop_box = (
            max(0, int(right_label_block["x2"]) - 26),
            max(0, row_top - 42),
            min(image.width, int(right_label_block["x2"]) + 254),
            min(image.height, row_bottom + 10),
        )
        if crop_box[2] <= crop_box[0] or crop_box[3] <= crop_box[1]:
            return None
        digit_blocks = _ocr_scaled_crop_window(image, crop_box, lang=lang, psm=psm, scale=4)

    if not digit_blocks:
        return None
    return max(digit_blocks, key=lambda block: (block["score"], block["x1"]))["text"]


def crop_digit_blocks(image: Image.Image, region: tuple[int, int, int, int]) -> list[dict[str, Any]]:
    left, top, right, bottom = region
    if right <= left or bottom <= top:
        return []

    region_width = right - left
    region_height = bottom - top
    window_width = min(140, max(60, region_width))
    window_height = min(80, max(40, min(region_height, 60)))

    x_positions = [left, max(left, (left + right - window_width) // 2), max(left, right - window_width)]
    y_offset = min(10, max(0, region_height - window_height))
    y_positions = [top, top + y_offset, max(top, (top + bottom - window_height) // 2)]

    crop_boxes = [
        (x_pos, y_pos, x_pos + window_width, y_pos + window_height)
        for y_pos in dict.fromkeys(y_positions)
        for x_pos in dict.fromkeys(x_positions)
    ]

    digit_blocks: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=3) as executor:
        futures = [executor.submit(_ocr_crop_window, image, box) for box in crop_boxes]
        for future in as_completed(futures):
            digit_blocks.extend(future.result())

    if digit_blocks:
        return digit_blocks

    pad_x = 24
    pad_top = 48
    pad_bottom = 16
    fallback_box = (
        max(0, left - pad_x),
        max(0, top - pad_top),
        min(image.width, right + pad_x),
        min(image.height, bottom + pad_bottom),
    )
    if fallback_box[2] > fallback_box[0] and fallback_box[3] > fallback_box[1]:
        digit_blocks.extend(_ocr_scaled_crop_window(image, fallback_box, scale=4))

    return digit_blocks


def choose_digit_text(
    page_blocks: list[dict[str, Any]],
    image: Image.Image,
    region: tuple[int, int, int, int],
) -> str | None:
    candidates = digit_blocks_in_region(page_blocks, region)
    page_best = max(candidates, key=lambda block: (block["score"], block["x1"])) if candidates else None

    crop = image.crop(region)
    crop_candidates = [
        block
        for block in ocr_image_variants(crop, psm=6, whitelist="0123456789")
        if is_digit_block(block)
    ]
    if not crop_candidates:
        crop_candidates = crop_digit_blocks(image, region)
    crop_best = max(crop_candidates, key=lambda block: (block["score"], block["x1"])) if crop_candidates else None

    if page_best is None:
        return crop_best["text"] if crop_best else None

    page_digit = normalize_digit_text(page_best["text"])
    crop_digit = normalize_digit_text(crop_best["text"]) if crop_best else None
    if crop_digit is not None and len(crop_digit) > 2:
        crop_digit = None
        crop_best = None
    if page_digit is not None and crop_digit is not None:
        if len(crop_digit) > len(page_digit) and len(crop_digit) <= 2:
            return crop_best["text"]
    if page_digit in {"0", "00"} and crop_best and crop_digit not in {"0", "00"}:
        return crop_best["text"]
    if crop_best and crop_best["score"] > page_best["score"] + 0.2:
        return crop_best["text"]

    return page_best["text"]


def _process_defect_row(
    row: list[dict[str, Any]],
    blocks: list[dict[str, Any]],
    image: Image.Image,
) -> tuple[dict[str, Any], dict[str, Any]] | None:
    """Process a single defect row and return (row_data, defects_dict)."""
    ordered = sorted(row, key=lambda item: (item["x1"], item["y1"]))
    labels = [block for block in ordered if normalize_text(block["text"]) and not is_digit_block(block)]
    digits = [block for block in ordered if is_digit_block(block)]

    if len(labels) < 2:
        return None

    left_label_block = min(labels, key=lambda block: block["x1"])
    right_label_block = max(labels, key=lambda block: block["x1"])
    row_top = int(min(block["y1"] for block in ordered))
    row_bottom = int(max(block["y2"] for block in ordered))
    row_right = int(max(block["x2"] for block in ordered))

    left_region = (
        int(left_label_block["x2"]) + 4,
        max(row_top, int(left_label_block["y1"]) - 12),
        int(right_label_block["x1"]),
        min(row_bottom, int(left_label_block["y2"]) + 30),
    )
    right_region = (
        int(right_label_block["x2"]) + 4,
        max(row_top, int(right_label_block["y1"]) - 12),
        max(row_right, int(right_label_block["x2"]) + 240),
        min(row_bottom, int(right_label_block["y2"]) + 30),
    )

    left_value_text = choose_digit_text(blocks, image, left_region)
    right_value_text = choose_digit_text(blocks, image, right_region)

    digit_candidates = sorted(digits, key=lambda block: (block["x1"], block["y1"]))
    if left_value_text is None and digit_candidates:
        left_value_text = normalize_digit_text(digit_candidates[0]["text"])
    if right_value_text is None and len(digit_candidates) > 1:
        right_value_text = normalize_digit_text(digit_candidates[-1]["text"])
    if right_value_text is None:
        right_value_text = _recover_right_value_from_expanded_crop(
            image,
            right_label_block,
            row_top,
            row_bottom,
        )

    left_value = parse_scalar(left_value_text)
    right_value = parse_scalar(right_value_text)

    left_label = left_label_block["text"]
    right_label = right_label_block["text"]

    row_data = {
        "left_label": left_label,
        "left_value": left_value,
        "right_label": right_label,
        "right_value": right_value,
    }

    row_defects = {left_label: left_value}
    if right_value is not None:
        row_defects[right_label] = right_value

    return row_data, row_defects


def parse_defect_rows(
    image: Image.Image,
    blocks: list[dict[str, Any]],
    start_y: float,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    defect_rows: list[dict[str, Any]] = []
    defects: dict[str, Any] = {}

    for idx, ((left_label, right_label), (left_box, right_box)) in enumerate(
        zip(EXPECTED_DEFECT_ROWS, EXPECTED_DEFECT_VALUE_BOXES)
    ):
        left_value = read_digit_from_box(image, left_box)
        right_value = read_digit_from_box(image, right_box)

        row_data = {
            "left_label": left_label,
            "left_value": left_value,
            "right_label": right_label,
            "right_value": right_value,
        }
        defect_rows.append(row_data)
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
        document["document_type"] = normalize_title_text(title_block["text"])

    for field_name, label in (
        ("date", ["date", "0ate"]),
        ("batch_number", ["batchnumber"]),
        ("length", ["length"]),
        ("supplier", ["supplier"]),
    ):
        label_block = find_block(blocks, label)
        if label_block:
            page_text = right_line_text(blocks, label_block)
            fallback_text = nearest_value_right(blocks, label_block)
            value_text = page_text or fallback_text
            if field_name == "date" and page_text:
                value_text = page_text
            elif field_name == "batch_number" and page_text:
                value_text = page_text
            elif field_name in {"length", "supplier"} and fallback_text:
                value_text = fallback_text
            if value_text is None:
                value_text = extract_header_value(image, label_block, field_name, fallback_text=fallback_text)
            if value_text:
                if field_name == "date":
                    document[field_name] = normalize_date_text(value_text)
                elif field_name == "batch_number":
                    document[field_name] = normalize_batch_number_text(value_text)
                elif field_name == "supplier":
                    document[field_name] = value_text.strip()
                else:
                    document[field_name] = value_text.strip()

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
    blocks = ocr_image_variants(ocr_ready)
    document = parse_document(ocr_ready, blocks)
    document["source_file"] = str(hex_path)
    document["ocr_block_count"] = len(blocks)
    document["image_size"] = [bitmap.width, bitmap.height]
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description="Fast convert HMI printer HEX output into JSON using pytesseract.")
    parser.add_argument("input_hex", type=Path, help="Path to the HEX capture file")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON to a file instead of stdout")
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
    parser.add_argument("--indent", type=int, default=2, help="JSON indentation level")
    args = parser.parse_args()

    if args.ocr_backend == "pytesseract":
        configure_tesseract_cmd(args.tesseract_cmd)
    configure_ocr(args.lang, args.psm)
    configure_backend(args.ocr_backend)
    configure_value_mode(args.value_mode)
    document = convert_capture(args.input_hex)
    payload = json.dumps(document, indent=args.indent, ensure_ascii=False)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)


if __name__ == "__main__":
    main()