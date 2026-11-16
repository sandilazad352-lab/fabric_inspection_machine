#!/usr/bin/env python3
"""
Convert HMI Printer HEX raster capture to PDF.

Reads ESC/P thermal printer raster format, reconstructs bitmap, optionally 
preprocesses for OCR readability, and exports as PDF.

Usage:
    python hmi_hex_to_pdf.py <HEX_FILE> -o output.pdf [--ocr-ready]

Options:
    --ocr-ready: Apply OCR preprocessing (grayscale, scale 4x, autocontrast, invert)
                 For normal document capture, omit this flag.
"""

import argparse
import sys
from pathlib import Path
from PIL import Image, ImageOps


def read_hex_text(filepath):
    """
    Read HEX text file and return whitespace-stripped hex string.
    
    Args:
        filepath: Path to .hex file
        
    Returns:
        str: Whitespace-stripped hex content
    """
    with open(filepath, 'r', encoding='utf-8') as f:
        return "".join(f.read().split())


def hex_to_bitmap(hex_text):
    """
    Decode ESC/P thermal printer raster into bitmap.
    
    Uses marker "0D0A1B4BF000" to split rows and reconstruct 1-bit image.
    
    Args:
        hex_text: Whitespace-stripped hex string from file
        
    Returns:
        PIL.Image: 1-bit (mode '1') bitmap image
    """
    MARKER = "0D0A1B4BF000"
    ROW_BYTES = 240
    BITS_PER_BYTE = 8
    
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


def preprocess_for_ocr(image, scale=4):
    """
    Preprocess 1-bit image for OCR (optional, improves readability).
    
    Pipeline: 1-bit → grayscale → upscale → autocontrast → invert
    
    Args:
        image: PIL.Image (1-bit or L-mode)
        scale: Upscaling factor (default 4)
        
    Returns:
        PIL.Image: Preprocessed L-mode image
    """
    if image.mode == '1':
        image = image.convert('L')
    else:
        image = image.convert('L')
    
    new_width = image.width * scale
    new_height = image.height * scale
    image = image.resize((new_width, new_height), Image.Resampling.LANCZOS)
    
    image = ImageOps.autocontrast(image)
    image = ImageOps.invert(image.convert('L'))
    
    return image


def convert_hex_to_pdf(hex_filepath, output_pdf, ocr_ready=False):
    """
    Read HEX file, convert to bitmap, optionally preprocess, save as PDF.
    
    Args:
        hex_filepath: Path to input .hex file
        output_pdf: Path to output .pdf file
        ocr_ready: If True, apply OCR preprocessing before saving
        
    Returns:
        None
    """
    hex_path = Path(hex_filepath)
    pdf_path = Path(output_pdf)
    
    if not hex_path.exists():
        raise FileNotFoundError(f"HEX file not found: {hex_path}")
    
    print(f"Reading HEX from: {hex_path}")
    hex_text = read_hex_text(hex_path)
    
    print("Converting HEX to bitmap...")
    img = hex_to_bitmap(hex_text)
    
    if ocr_ready:
        print("Preprocessing for OCR...")
        img = preprocess_for_ocr(img, scale=4)
    
    # Convert to RGB for PDF compatibility (PDF doesn't support 1-bit directly)
    if img.mode == '1':
        img = img.convert('RGB')
    elif img.mode == 'L':
        img = img.convert('RGB')
    
    print(f"Saving PDF to: {pdf_path}")
    img.save(pdf_path, 'PDF')
    
    print(f"✓ PDF created successfully: {pdf_path}")
    print(f"  Image size: {img.width}×{img.height} pixels")


def main():
    parser = argparse.ArgumentParser(
        description='Convert HMI printer HEX capture to PDF',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  python hmi_hex_to_pdf.py HMI_CAPTURE_20260409_173342.hex -o output.pdf
  python hmi_hex_to_pdf.py input.hex -o output.pdf --ocr-ready
        '''
    )
    
    parser.add_argument('hex_file', help='Input HEX file')
    parser.add_argument('-o', '--output', dest='output', required=True, help='Output PDF file')
    parser.add_argument('--ocr-ready', action='store_true', 
                        help='Apply OCR preprocessing (upscale and enhance)')
    
    args = parser.parse_args()
    
    try:
        convert_hex_to_pdf(args.hex_file, args.output, ocr_ready=args.ocr_ready)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
