#!/usr/bin/env python3
"""
HMI to PC Serial HEX Capture Tool
Reads raw RS-232 thermal printer data from the fabric inspection HMI,
prints protocol diagnostics, and saves uppercase HEX (.hex) files
compatible with the fabric_inspection_machine decoders.
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

import serial
import serial.tools.list_ports

ROW_BYTES = 240
BITS_PER_BYTE = 8
STRIPE_MARKER = "0D0A1B4BF000"
FIRST_STRIPE_MARKER = "1B4BF000"


class HMIDataCapture:
    """Capture RS-232 data from HMI and save formatted HEX files."""

    def __init__(self, port: str, baudrate: int = 9600, timeout: float = 2.0) -> None:
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial: Optional[serial.Serial] = None

    @property
    def is_connected(self) -> bool:
        return self.serial is not None and self.serial.is_open

    def connect(self) -> bool:
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.timeout,
            )
            print(f"[OK] Connected to {self.port} @ {self.baudrate} bps (8N1)")
            return True
        except serial.SerialException as exc:
            print(f"[ERR] Serial connection failed on {self.port}: {exc}")
            return False

    def disconnect(self) -> None:
        if self.serial and self.serial.is_open:
            self.serial.close()
            print(f"[OK] Disconnected from {self.port}")
        self.serial = None

    def read_hex_data(
        self,
        max_bytes: int = 65536,
        timeout_sec: float = 60.0,
        idle_sec: float = 2.0,
    ) -> Optional[bytes]:
        """Read a single print burst from the serial port until idle gap or timeout."""
        if not self.is_connected or self.serial is None:
            print("[ERR] Serial port not connected")
            return None

        wait_info = "forever (Ctrl+C to stop)" if timeout_sec <= 0 else f"{timeout_sec:.0f}s"
        print(f"\nWaiting for HMI serial data (timeout: {wait_info}, idle gap: {idle_sec:.1f}s)...")

        start_time = time.monotonic()
        last_data_time = start_time
        chunks: list[bytes] = []
        total_bytes = 0

        try:
            while timeout_sec <= 0 or (time.monotonic() - start_time) < timeout_sec:
                waiting = self.serial.in_waiting
                if waiting > 0:
                    chunk = self.serial.read(min(waiting, 1024))
                    if chunk:
                        chunks.append(chunk)
                        total_bytes += len(chunk)
                        last_data_time = time.monotonic()
                        elapsed = last_data_time - start_time
                        print(f"  [{elapsed:5.1f}s] Captured: {total_bytes} bytes", end="\r", flush=True)

                if total_bytes > 0 and (time.monotonic() - last_data_time) >= idle_sec:
                    print(f"\n[OK] Idle gap ({idle_sec:.1f}s) reached; capture complete.")
                    break

                if total_bytes >= max_bytes:
                    print(f"\n[OK] Reached max capture size ({max_bytes} bytes).")
                    break

                time.sleep(0.02)
            else:
                if total_bytes == 0:
                    print(f"\n[WARN] No data received within {timeout_sec:.0f}s.")
                    return None
                print(f"\n[OK] Timeout reached with {total_bytes} bytes captured.")

        except serial.SerialException as exc:
            print(f"\n[ERR] Serial read error: {exc}")
            return None

        if total_bytes == 0:
            return None

        data = b"".join(chunks)
        elapsed_total = max(time.monotonic() - start_time, 0.001)
        print(f"  Total bytes:   {len(data)}")
        print(f"  Elapsed time:  {elapsed_total:.2f}s")
        print(f"  Transfer rate: {len(data) / elapsed_total:.0f} bytes/sec")
        return data

    @staticmethod
    def format_hex(data: bytes, line_width: int = 80) -> str:
        hex_string = data.hex().upper()
        return "\n".join(hex_string[i : i + line_width] for i in range(0, len(hex_string), line_width))

    @staticmethod
    def analyze_data(data: bytes) -> None:
        hex_str = data.hex().upper()
        checks = {
            "ESC @ (Initialize / 1B40)": "1B40" in hex_str,
            "ESC K (Bitmap / 1B4B)": "1B4B" in hex_str,
            "CR (0D)": "0D" in hex_str,
            "LF (0A)": "0A" in hex_str,
            "ESC l (Left Margin / 1B6C)": "1B6C" in hex_str,
            "ESC 1 (Line Spacing / 1B31)": "1B31" in hex_str,
        }
        print("\nData Analysis:")
        for label, found in checks.items():
            print(f"  {label}: {'FOUND' if found else 'not found'}")
        print(f"  ESC K commands:   {hex_str.count('1B4B')}")
        print(f"  First 40 bytes:   {data[:40].hex().upper()}")
        print(f"  Last 40 bytes:    {data[-40:].hex().upper()}")

    @classmethod
    def save_hex_file(cls, data: bytes, output_path: Path) -> Path:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        formatted = cls.format_hex(data)
        output_path.write_text(formatted, encoding="utf-8")
        print(f"[OK] Saved HEX: {output_path} ({output_path.stat().st_size} bytes)")
        return output_path


def render_png_from_hex(hex_text: str, output_png: Path, scale: int = 4) -> Optional[Path]:
    """Optionally render captured ESC/P raster HEX to PNG if Pillow is available."""
    try:
        from PIL import Image
    except ImportError:
        print("[WARN] Pillow is not installed; skipping PNG preview.")
        return None

    clean_hex = "".join(hex_text.split())
    segments: list[bytes] = []

    # Preserve optional initial stripe before the first CR+LF (e.g., 1B401B31001B4BF000...)
    parts = clean_hex.split(STRIPE_MARKER)
    if parts and FIRST_STRIPE_MARKER in parts[0]:
        first_tail = parts[0].split(FIRST_STRIPE_MARKER, 1)[1]
        if len(first_tail) == ROW_BYTES * 2:
            segments.append(bytes.fromhex(first_tail))

    for part in parts[1:]:
        if len(part) == ROW_BYTES * 2:
            segments.append(bytes.fromhex(part))

    if not segments:
        print("[WARN] No 240-byte ESC K raster stripes found; skipping PNG preview.")
        return None

    image = Image.new("1", (ROW_BYTES, len(segments) * BITS_PER_BYTE), 1)
    for row_index, segment in enumerate(segments):
        for x_index, byte_value in enumerate(segment):
            draw_x = ROW_BYTES - 1 - x_index
            for bit_index in range(BITS_PER_BYTE):
                if not (byte_value & (1 << bit_index)):
                    image.putpixel((draw_x, row_index * BITS_PER_BYTE + bit_index), 0)

    if scale > 1:
        image = image.resize((image.width * scale, image.height * scale), Image.Resampling.NEAREST)

    output_png.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_png)
    print(f"[OK] Saved PNG: {output_png} ({image.width}x{image.height})")
    return output_png


def list_serial_ports() -> list[tuple[str, str, str]]:
    return [(p.device, p.description, p.hwid) for p in serial.tools.list_ports.comports()]


def choose_port_interactive() -> Optional[str]:
    ports = list_serial_ports()
    if not ports:
        print("[ERR] No serial (COM) ports detected.")
        return None

    print("\nAvailable serial ports:")
    for idx, (device, desc, _) in enumerate(ports, start=1):
        print(f"  {idx}) {device} - {desc}")

    choice = input("\nSelect port number or name (e.g. 1 or COM9): ").strip()
    if not choice:
        return ports[0][0]
    if choice.isdigit():
        idx = int(choice) - 1
        if 0 <= idx < len(ports):
            return ports[idx][0]
        print("[ERR] Invalid port number.")
        return None
    return choice.upper() if choice.lower().startswith("com") else choice


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Capture RS-232 data from HMI to PC as HEX.")
    parser.add_argument("-p", "--port", help="Serial port (e.g. COM9). Prompts if omitted.")
    parser.add_argument("-b", "--baud", type=int, default=9600, help="Baud rate (default: 9600)")
    parser.add_argument("-t", "--timeout", type=float, default=60.0, help="Wait timeout in seconds (0 = infinite)")
    parser.add_argument("--idle-timeout", type=float, default=2.0, help="Idle gap in seconds to end capture")
    parser.add_argument("--max-bytes", type=int, default=65536, help="Maximum bytes per capture")
    parser.add_argument("-o", "--output", type=Path, help="Explicit output .hex file path (single-shot)")
    parser.add_argument("--out-dir", type=Path, default=Path("captures"), help="Directory for auto-named captures")
    parser.add_argument("--continuous", action="store_true", help="Keep listening for multiple HMI captures")
    parser.add_argument("--png", action="store_true", help="Also render PNG alongside saved .hex")
    parser.add_argument("--list-ports", action="store_true", help="List available COM ports and exit")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    if args.list_ports:
        ports = list_serial_ports()
        if not ports:
            print("No serial ports found.")
            return 1
        for device, desc, hwid in ports:
            print(f"{device}\t{desc}\t{hwid}")
        return 0

    port = args.port or choose_port_interactive()
    if not port:
        return 1

    capture = HMIDataCapture(port=port, baudrate=args.baud)
    if not capture.connect():
        return 1

    try:
        while True:
            wait_timeout = 0.0 if args.continuous and args.timeout == 60.0 else args.timeout
            data = capture.read_hex_data(
                max_bytes=args.max_bytes,
                timeout_sec=wait_timeout,
                idle_sec=args.idle_timeout,
            )

            if data is None:
                if args.continuous:
                    continue
                return 1

            capture.analyze_data(data)

            if args.output and not args.continuous:
                out_hex = args.output
            else:
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                out_hex = args.out_dir / f"HMI_CAPTURE_{timestamp}.hex"

            capture.save_hex_file(data, out_hex)

            if args.png:
                render_png_from_hex(capture.format_hex(data), out_hex.with_suffix(".png"))

            if not args.continuous:
                return 0
    except KeyboardInterrupt:
        print("\nStopped by user.")
        return 0
    finally:
        capture.disconnect()


if __name__ == "__main__":
    sys.exit(main())
