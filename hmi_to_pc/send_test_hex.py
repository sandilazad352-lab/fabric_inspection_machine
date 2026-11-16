#!/usr/bin/env python3
"""
Simulate HMI RS-232 output from PC by streaming a .hex capture file
to a USB-to-Serial port (for testing ESP32-C5 without the physical HMI).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial


def load_hex_bytes(hex_path: Path) -> bytes:
    cleaned = "".join(hex_path.read_text(encoding="utf-8").split())
    return bytes.fromhex(cleaned)


def send_bytes(port: str, baud: int, data: bytes, chunk_size: int = 256, delay_sec: float = 0.01) -> None:
    with serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=2.0,
    ) as ser:
        print(f"[OK] Opened {port} @ {baud} 8N1, sending {len(data)} bytes...")
        sent = 0
        for i in range(0, len(data), chunk_size):
            chunk = data[i : i + chunk_size]
            sent += ser.write(chunk)
            ser.flush()
            if delay_sec > 0:
                time.sleep(delay_sec)
        print(f"[OK] Sent {sent}/{len(data)} bytes.")


def main() -> int:
    default_hex = (
        Path(__file__).resolve().parent.parent
        / "fabric_inspection_machine-main"
        / "fabric_inspection_machine-main"
        / "HMI_CAPTURE_20260409_173342.hex"
    )

    parser = argparse.ArgumentParser(description="Send a .hex capture over serial to simulate the HMI.")
    parser.add_argument("-p", "--port", required=True, help="USB-to-Serial COM port (e.g. COM48)")
    parser.add_argument("-b", "--baud", type=int, default=9600, help="Baud rate (default: 9600)")
    parser.add_argument("-f", "--file", type=Path, default=default_hex, help="Input .hex file to transmit")
    parser.add_argument("--chunk-size", type=int, default=256, help="Bytes per write chunk (default: 256)")
    parser.add_argument("--delay", type=float, default=0.01, help="Delay between chunks in seconds (default: 0.01)")
    args = parser.parse_args()

    if not args.file.exists():
        print(f"[ERR] HEX file not found: {args.file}", file=sys.stderr)
        return 1

    data = load_hex_bytes(args.file)
    send_bytes(args.port, args.baud, data, chunk_size=args.chunk_size, delay_sec=args.delay)
    return 0


if __name__ == "__main__":
    sys.exit(main())
