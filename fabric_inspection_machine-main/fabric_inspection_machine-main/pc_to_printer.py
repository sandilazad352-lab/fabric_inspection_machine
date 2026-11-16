#!/usr/bin/env python3
"""
PC to Printer - Send HEX File to Thermal Printer
Purpose: Send captured HEX file from PC to BRIGHTEK printer
Single RS232 connection to printer only
Author: Generated for PC → Printer transmission
Date: April 9, 2026
"""

import serial
import time
import sys
import os
import glob
from typing import Optional


class PrinterTransmission:
    """Send HEX file to BRIGHTEK thermal printer"""
    
    def __init__(self, printer_port: str = 'COM9', baudrate: int = 9600):
        """
        Initialize printer transmission system
        
        Args:
            printer_port: Port connected to printer
            baudrate: Baud rate (default: 9600)
        """
        self.printer_port = printer_port
        self.baudrate = baudrate
        self.serial = None
        self.is_connected = False
        self.hex_data = None
    
    def connect(self) -> bool:
        """Connect to printer"""
        try:
            self.serial = serial.Serial(
                port=self.printer_port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=2.0
            )
            self.is_connected = True
            print(f"✓ Connected to Printer on {self.printer_port} @ {self.baudrate} bps")
            return True
        except serial.SerialException as e:
            print(f"✗ Printer connection failed: {e}")
            return False
    
    def disconnect(self) -> None:
        """Disconnect from printer"""
        if self.serial and self.serial.is_open:
            self.serial.close()
            self.is_connected = False
            print("✓ Printer disconnected")
    
    def load_hex_file(self, filename: str) -> Optional[bytes]:
        """
        Load HEX file and convert to bytes
        
        Args:
            filename: Path to HEX file
        
        Returns:
            bytes: Converted data, or None if error
        """
        if not os.path.exists(filename):
            print(f"✗ File not found: {filename}")
            return None
        
        try:
            # Read HEX file
            with open(filename, 'r') as f:
                content = f.read()
            
            # Remove whitespace (spaces, newlines, tabs)
            hex_string = ''.join(content.split())
            
            print(f"✓ Loaded HEX file: {os.path.basename(filename)}")
            print(f"  File size: {len(content)} chars")
            print(f"  Cleaned: {len(hex_string)} chars")
            
            # Convert to bytes
            try:
                byte_data = bytes.fromhex(hex_string)
                print(f"  Bytes: {len(byte_data)}")
                return byte_data
            except ValueError as e:
                print(f"✗ Invalid HEX format: {e}")
                return None
        
        except IOError as e:
            print(f"✗ File read error: {e}")
            return None
    
    def analyze_hex(self, data: bytes) -> None:
        """Analyze HEX data before sending"""
        if not data:
            return
        
        print(f"\n📊 Data Analysis:")
        
        hex_str = data.hex().upper()
        
        # Check for commands
        checks = {
            'ESC @ (Initialize)': '1B40' in hex_str,
            'ESC K (Bitmap)': '1B4B' in hex_str,
            'CR (Carriage Return)': '0D' in hex_str,
            'LF (Line Feed)': '0A' in hex_str,
        }
        
        print(f"   Command Detection:")
        for cmd, found in checks.items():
            status = "✓ Found" if found else "⚠ Not found"
            print(f"     {cmd}: {status}")
        
        # Count ESC K commands
        esc_k_count = hex_str.count('1B4B')
        print(f"\n   Graphics Commands (ESC K): {esc_k_count}")
        print(f"   First 40 bytes: {data[:40].hex().upper()}")
    
    def send_to_printer(self, data: bytes, chunk_size: int = 256, 
                       delay: float = 0.005) -> bool:
        """
        Send data to printer
        
        Args:
            data: Bytes to send
            chunk_size: Send in chunks
            delay: Delay between chunks
        
        Returns:
            bool: True if successful
        """
        if not self.is_connected:
            print("✗ Printer not connected")
            return False
        
        if not data:
            print("✗ No data to send")
            return False
        
        try:
            print(f"\n📤 Sending to Printer...")
            print(f"   Total bytes: {len(data)}")
            print(f"   Chunk size: {chunk_size}")
            
            # Send initialization commands first
            print(f"\n📌 Sending printer initialization...")
            
            # Initialize printer (ESC @)
            init_cmd = bytes([0x1B, 0x40])
            self.serial.write(init_cmd)
            time.sleep(0.3)
            
            # Set left margin (ESC l 0)
            left_margin = bytes([0x1B, 0x6C, 0x00])
            self.serial.write(left_margin)
            time.sleep(0.2)
            
            # Set line spacing (ESC 1 3)
            line_spacing = bytes([0x1B, 0x31, 0x03])
            self.serial.write(line_spacing)
            time.sleep(0.2)
            
            # Feed lines for positioning
            for _ in range(3):
                self.serial.write(bytes([0x0A]))
                time.sleep(0.05)
            
            print(f"✓ Printer initialized")
            
            # Send main data
            print(f"\n   Transmitting data...")
            
            total_sent = 0
            for i in range(0, len(data), chunk_size):
                chunk = data[i:i + chunk_size]
                sent = self.serial.write(chunk)
                total_sent += sent
                
                percent = (total_sent / len(data)) * 100
                print(f"   Progress: {percent:.1f}% ({total_sent}/{len(data)} bytes)", end='\r')
                
                if delay > 0:
                    time.sleep(delay)
            
            self.serial.flush()
            print(f"✓ Sent {total_sent} bytes to printer                    ")
            
            return total_sent == len(data)
        
        except serial.SerialException as e:
            print(f"✗ Send error: {e}")
            return False
    
    def verify_printer(self) -> None:
        """Wait and check for printer response"""
        print(f"\n⏳ Waiting for printer to process (5 seconds)...")
        time.sleep(5)
        
        try:
            if self.serial.in_waiting > 0:
                response = self.serial.read(self.serial.in_waiting)
                print(f"✓ Printer responded with {len(response)} bytes")
        except:
            pass


def find_latest_hex_file() -> Optional[str]:
    """Find the most recent HMI_CAPTURE_*.hex file"""
    files = glob.glob("HMI_CAPTURE_*.hex")
    
    if not files:
        return None
    
    # Sort by modification time, get most recent
    latest = max(files, key=os.path.getctime)
    return latest


def select_hex_file() -> Optional[str]:
    """Interactive HEX file selection"""
    
    print("\n" + "="*60)
    print("HEX FILE SELECTION")
    print("="*60)
    
    # Find available files
    files = glob.glob("HEX_CAPTURE_*.hex") + glob.glob("SAMPLE*.hex")
    
    if not files:
        print("\n⚠️  No HEX files found in current directory")
        print("\nEnter HEX filename: ", end='')
        filename = input().strip()
        return filename if filename else None
    
    print(f"\nFound {len(files)} HEX file(s):")
    for i, f in enumerate(files, 1):
        size = os.path.getsize(f)
        print(f"  {i}) {f} ({size:,} bytes)")
    
    # Get most recent
    latest = max(files, key=os.path.getctime)
    print(f"\nMost recent: {latest}")
    print(f"\nUse this file? (Y/n): ", end='')
    choice = input().strip().lower()
    
    if choice == 'n':
        print("Enter HEX filename: ", end='')
        return input().strip()
    
    return latest


def select_printer_port() -> str:
    """Interactive printer port selection"""
    import serial.tools.list_ports
    
    print("\n" + "="*60)
    print("PRINTER PORT SELECTION")
    print("="*60)
    
    ports = []
    print("\nAvailable serial ports:")
    for port, desc, hwid in serial.tools.list_ports.comports():
        ports.append(port)
        print(f"  {port}: {desc}")
    
    if not ports:
        print("\n✗ No serial ports found!")
        return None
    
    print(f"\nEnter Printer port [default: COM3]: ", end='')
    printer_port = input().strip() or 'COM3'
    
    return printer_port


def main():
    """Main execution"""
    
    print("\n" + "="*60)
    print("PC to Printer - HEX File Transmission")
    print("="*60)
    
    print("\nThis system sends HEX files to your thermal printer")
    
    # Step 1: Select HEX file
    print(f"\n" + "="*60)
    print("STEP 1: Select HEX File")
    print("="*60)
    
    hex_file = select_hex_file()
    if not hex_file:
        print("\n✗ No HEX file selected")
        return False
    
    # Step 2: Select printer port
    print(f"\n" + "="*60)
    print("STEP 2: Select Printer Port")
    print("="*60)
    
    printer_port = select_printer_port()
    if not printer_port:
        return False
    
    print(f"\n📍 Configuration:")
    print(f"   HEX File:      {hex_file}")
    print(f"   Printer Port:  {printer_port}")
    print(f"   Baud Rate:     9600 bps")
    
    # Create transmission
    transmission = PrinterTransmission(printer_port)
    
    # Connect
    print(f"\n" + "="*60)
    print("Connecting to Printer...")
    print("="*60)
    print()
    
    if not transmission.connect():
        return False
    
    try:
        # Step 3: Load HEX file
        print(f"\n" + "="*60)
        print("STEP 2: Loading HEX File")
        print("="*60)
        print()
        
        transmission.hex_data = transmission.load_hex_file(hex_file)
        if not transmission.hex_data:
            return False
        
        # Step 4: Analyze
        print(f"\n" + "="*60)
        print("STEP 3: Analyzing Data")
        print("="*60)
        
        transmission.analyze_hex(transmission.hex_data)
        
        # Step 5: Send
        print(f"\n" + "="*60)
        print("STEP 4: Sending to Printer")
        print("="*60)
        
        if not transmission.send_to_printer(transmission.hex_data):
            return False
        
        # Step 6: Verify
        transmission.verify_printer()
        
        # Completion
        print(f"\n" + "="*60)
        print("✓ TRANSMISSION COMPLETE")
        print("="*60)
        
        print(f"\n📋 Next steps:")
        print(f"   1. Check thermal printer output")
        print(f"   2. Verify all fields printed correctly")
        print(f"   3. Process complete! ✓")
        
        return True
    
    except KeyboardInterrupt:
        print("\n\n✗ Operation interrupted by user")
        return False
    
    finally:
        transmission.disconnect()


if __name__ == "__main__":
    try:
        success = main()
        sys.exit(0 if success else 1)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
