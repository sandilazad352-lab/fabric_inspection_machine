#!/usr/bin/env python3
"""
HMI to PC HEX Data Capture System
Purpose: Read HEX data from HMI and save to PC file
Single RS232 connection to HMI only
Author: Generated for HMI data capture workflow
Date: April 9, 2026
"""

import serial
import time
import sys
import os
from datetime import datetime
from typing import Optional


class HMIDataCapture:
    """Capture HEX data from HMI and save to file"""
    
    def __init__(self, hmi_port: str = 'COM9', baudrate: int = 9600):
        """
        Initialize HMI capture system
        
        Args:
            hmi_port: Port connected to HMI
            baudrate: Baud rate (default: 9600)
        """
        self.hmi_port = hmi_port
        self.baudrate = baudrate
        self.serial = None
        self.is_connected = False
        self.captured_data = None
    
    def connect(self) -> bool:
        """Connect to HMI"""
        try:
            self.serial = serial.Serial(
                port=self.hmi_port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=2.0
            )
            self.is_connected = True
            print(f"✓ Connected to HMI on {self.hmi_port} @ {self.baudrate} bps")
            return True
        except serial.SerialException as e:
            print(f"✗ HMI connection failed: {e}")
            return False
    
    def disconnect(self) -> None:
        """Disconnect from HMI"""
        if self.serial and self.serial.is_open:
            self.serial.close()
            self.is_connected = False
            print("✓ HMI disconnected")
    
    def read_hex_data(self, max_bytes: int = 65536, timeout_sec: float = 60) -> Optional[bytes]:
        """
        Read HEX data from HMI
        
        Args:
            max_bytes: Maximum bytes to read
            timeout_sec: Total timeout in seconds
        
        Returns:
            bytes: Data read from HMI
        """
        if not self.is_connected:
            print("✗ HMI not connected")
            return None
        
        try:
            print(f"\n⏱️  Waiting for data from HMI...")
            print(f"   Timeout: {timeout_sec} seconds")
            print(f"   Max size: {max_bytes} bytes")
            print(f"\n   Starting capture now...")
            print(f"   (HMI should send data...)\n")
            
            start_time = time.time()
            data = b''
            last_data_time = time.time()
            
            while time.time() - start_time < timeout_sec:
                if self.serial.in_waiting > 0:
                    # Read available bytes
                    chunk = self.serial.read(min(self.serial.in_waiting, 1024))
                    if chunk:
                        data += chunk
                        last_data_time = time.time()
                        
                        # Progress indicator
                        elapsed = time.time() - start_time
                        print(f"   [{elapsed:.1f}s] Captured: {len(data)} bytes...", end='\r')
                
                # If no new data for 2 seconds, consider complete
                if data and (time.time() - last_data_time) > 2.0:
                    print(f"\n   (No new data for 2 seconds - capture complete)")
                    break
                
                # Check against max bytes
                if len(data) >= max_bytes:
                    print(f"\n   (Reached maximum size)")
                    break
                
                time.sleep(0.05)
            
            elapsed_total = time.time() - start_time
            
            if data:
                print(f"\n✓ Capture complete!")
                print(f"  Total time: {elapsed_total:.2f} seconds")
                print(f"  Total bytes: {len(data)}")
                print(f"  Transfer rate: {len(data)/(elapsed_total or 1):.0f} bytes/sec")
                return data
            else:
                print(f"\n✗ No data received from HMI")
                print(f"  Waited {timeout_sec} seconds with no response")
                return None
        
        except serial.SerialException as e:
            print(f"✗ Read error: {e}")
            return None
    
    def save_to_file(self, data: bytes, filename: Optional[str] = None) -> bool:
        """
        Save captured data to file
        
        Args:
            data: Binary data to save
            filename: Output filename (auto-generated if None)
        
        Returns:
            bool: True if successful
        """
        if not data:
            print("✗ No data to save")
            return False
        
        # Generate filename if not provided
        if not filename:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"HMI_CAPTURE_{timestamp}.hex"
        
        try:
            # Convert bytes to hex string
            hex_string = data.hex().upper()
            
            # Format with line breaks (80 chars per line)
            formatted_hex = '\n'.join([
                hex_string[i:i+80] for i in range(0, len(hex_string), 80)
            ])
            
            # Save to file
            with open(filename, 'w') as f:
                f.write(formatted_hex)
            
            print(f"\n✓ Data saved to file!")
            print(f"  Filename: {filename}")
            print(f"  File size: {os.path.getsize(filename)} bytes")
            print(f"  Data size: {len(data)} bytes")
            
            return True
        
        except IOError as e:
            print(f"✗ Save error: {e}")
            return False
    
    def analyze_data(self) -> None:
        """Analyze captured data"""
        if not self.captured_data:
            print("✗ No data to analyze")
            return
        
        print(f"\n📊 Data Analysis:")
        
        hex_str = self.captured_data.hex().upper()
        
        # Check for printer commands
        checks = {
            'ESC @ (Initialize)': '1B40' in hex_str,
            'ESC K (Bitmap)': '1B4B' in hex_str,
            'CR (Carriage Return)': '0D' in hex_str,
            'LF (Line Feed)': '0A' in hex_str,
            'ESC l (Left Margin)': '1B6C' in hex_str,
            'ESC 1 (Line Spacing)': '1B31' in hex_str,
        }
        
        print(f"   Command Detection:")
        for cmd, found in checks.items():
            status = "✓ Found" if found else "⚠ Not found"
            print(f"     {cmd}: {status}")
        
        # Count ESC K commands
        esc_k_count = hex_str.count('1B4B')
        print(f"\n   Graphics Commands (ESC K): {esc_k_count}")
        
        # Show first and last bytes
        print(f"\n   Data Preview:")
        print(f"     First 40 bytes: {self.captured_data[:40].hex().upper()}")
        print(f"     Last 40 bytes:  {self.captured_data[-40:].hex().upper()}")


def select_port() -> str:
    """Interactive port selection"""
    import serial.tools.list_ports
    
    print("\n" + "="*60)
    print("SERIAL PORT SELECTION")
    print("="*60)
    
    ports = []
    print("\nAvailable serial ports:")
    for port, desc, hwid in serial.tools.list_ports.comports():
        ports.append(port)
        print(f"  {port}: {desc}")
    
    if not ports:
        print("\n✗ No serial ports found!")
        return None
    
    print(f"\nFound {len(ports)} port(s)")
    
    # Select HMI port
    print("\nEnter HMI port [default: COM1]: ", end='')
    hmi_port = input().strip() or 'COM1'
    
    return hmi_port


def main():
    """Main execution"""
    
    print("\n" + "="*60)
    print("HMI to PC - HEX Data Capture System")
    print("="*60)
    
    print("\nThis system reads HEX data from your HMI device")
    print("and saves it to your PC as a file")
    
    # Select port
    hmi_port = select_port()
    if not hmi_port:
        return False
    
    print(f"\n📍 Configuration:")
    print(f"   HMI Port:  {hmi_port}")
    print(f"   Baud Rate: 9600 bps")
    print(f"   Timeout:   60 seconds")
    
    # Create capture system
    capture = HMIDataCapture(hmi_port)
    
    # Connect
    print(f"\n" + "="*60)
    print("Connecting to HMI...")
    print("="*60)
    
    if not capture.connect():
        return False
    
    try:
        # Step 1: Read data
        print(f"\n" + "="*60)
        print("STEP 1: Capturing HEX Data from HMI")
        print("="*60)
        
        capture.captured_data = capture.read_hex_data(timeout_sec=60)
        
        if not capture.captured_data:
            print("\n✗ Failed to capture data from HMI")
            print("\nPossible causes:")
            print("  1. HMI is not sending data")
            print("  2. Wrong COM port selected")
            print("  3. HMI not powered on")
            print("  4. RS232 cable not connected")
            return False
        
        # Step 2: Analyze data
        print(f"\n" + "="*60)
        print("STEP 2: Analyzing Captured Data")
        print("="*60)
        
        capture.analyze_data()
        
        # Step 3: Save to file
        print(f"\n" + "="*60)
        print("STEP 3: Saving Data to File")
        print("="*60)
        
        if not capture.save_to_file(capture.captured_data):
            print("\n✗ Failed to save data")
            return False
        
        # Step 4: Completion
        print(f"\n" + "="*60)
        print("✓ CAPTURE COMPLETE")
        print("="*60)
        
        print(f"\n📋 Next steps:")
        print(f"   1. HEX file saved to current directory")
        print(f"   2. Use with printer_rs232.py to send to printer")
        print(f"   3. Or use with hmi_to_printer.py for auto-relay")
        
        return True
    
    except KeyboardInterrupt:
        print("\n\n✗ Operation interrupted by user")
        return False
    
    finally:
        # Cleanup
        capture.disconnect()


if __name__ == "__main__":
    try:
        success = main()
        sys.exit(0 if success else 1)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
