# HMI to PC Serial HEX Reader

Dedicated Python project for capturing RS-232 data from the fabric inspection machine HMI (or ESP32-C5 relay) to the PC as `.hex` files, based on `fabric_inspection_machine-main`.

## Setup Virtual Environment

```powershell
cd G:\fi_hex_json\fi_hex_json\hmi_to_pc
.\.venv\Scripts\Activate.ps1
```

If recreating from scratch:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

## Usage

List connected COM ports:

```powershell
python hmi_serial_to_pc.py --list-ports
```

Interactive single capture (prompts for COM port, 9600 8N1):

```powershell
python hmi_serial_to_pc.py
```

Direct single capture with PNG preview:

```powershell
python hmi_serial_to_pc.py --port COM9 --baud 9600 --png
```

Continuous capture mode (saves every print job under `captures\`):

```powershell
python hmi_serial_to_pc.py --port COM9 --continuous --png
```
