# Fabric Inspection Gateway — Backend & Frontend Developer Guide

Complete integration and API specification for the **ESP32-C5 RS-232-to-MQTT Industrial Gateway** (`fi_hex_json`).

---

## 1. System Architecture Overview

```text
+-------------------+   RS-232 (9600 8N1)   +------------------------+   MQTT (TCP/1883)   +----------------------+
| Fabric Inspection | --------------------> | ESP32-C5 Gateway       | ------------------> | Backend MQTT Worker  |
| Machine HMI       |  GPIO5 (RX) / GPIO4   | - UART1 Frame Capture  |  topic_hex (JSON)   | - Store HEX / Bitmap |
+-------------------+                       | - ESC/P Analyzer       |  topic_status (LWT) | - OCR / Parse JSON   |
                                            | - Web UI & REST API    | <------------------ | - Remote Cmds        |
                                            +------------------------+   topic_cmd         +----------------------+
                                                       ^
                                                       | HTTP (:80) + Cookie / X-Auth-Token
                                                       v
                                            +------------------------+
                                            | Frontend Web Dashboard |
                                            | (main/web/index.html)  |
                                            +------------------------+
```

* **Firmware Entry & Modules:**
  * `main/main.c` — Boot sequence (`nvs_flash`, `gateway_state`, `app_config`, `wifi_manager`, `mqtt_manager`, `rs232_reader`, `web_server`)
  * `main/app_config.h` / `main/app_config.c` — Persistent NVS configuration (`gw_config_t`)
  * `main/rs232_reader.c` — Dynamic `UART_NUM_1` reader (`2000ms` idle gap, max `32768` bytes)
  * `main/mqtt_manager.c` — MQTT client (`espressif/mqtt`), LWT, 30s heartbeat, telemetry publisher
  * `main/web_server.c` — HTTP server (`esp_http_server`) & session authentication
  * `main/web/index.html` — Embedded single-file frontend SPA + `<canvas>` receipt renderer
* **PC / Python Reference Tools:**
  * `hmi_to_pc/hmi_serial_to_pc.py` — Direct PC RS-232 capture & PNG renderer
  * `hmi_to_pc/send_test_hex.py` — Replay `.hex` capture files over serial to simulate the HMI
  * `fabric_inspection_machine-main/fabric_inspection_machine-main/` — HEX-to-PNG, HEX-to-PDF, and OCR/JSON converters

---

## 2. Backend Developer Reference (MQTT & Data Pipeline)

### 2.1 Default MQTT Topics

`<device_id>` defaults to `fi-gw-<last3bytes_mac>` (for example, `fi-gw-e011d8`). All topics are configurable at runtime via `POST /api/config` or the Web UI.

| Purpose | Direction | Default Topic | Retain |
| :--- | :--- | :--- | :--- |
| Inspection HEX Frame | Gateway -> Broker | `factory/inspection/<device_id>/hex` | Configurable (`false`) |
| Online / Offline Status | Gateway -> Broker | `factory/inspection/<device_id>/status` | `true` |
| Remote Command | Broker -> Gateway | `factory/inspection/<device_id>/cmd` | `false` |
| Debug Log Topic | Configurable | `factory/inspection/<device_id>/debug` | `false` |

### 2.2 Telemetry Payload (`topic_hex`)

Depending on `mqtt_format` (`0`, `1`, or `2`):

#### Mode `0`: JSON Envelope + HEX (Default, Recommended)

```json
{
  "device_id": "fi-gw-e011d8",
  "device_name": "FI-Inspection-Gateway",
  "location": "Inspection-Line-1",
  "seq": 1,
  "timestamp_ms": 71841,
  "bytes": 10340,
  "duration_ms": 11760,
  "rate_bps": 879,
  "esc_k_count": 42,
  "raster_stripes": 42,
  "reason": "idle_gap",
  "commands": {
    "esc_at": true,
    "esc_k": true,
    "cr": true,
    "lf": true,
    "esc_l": false,
    "esc_1": true
  },
  "hex": "1B401B31001B4BF000..."
}
```

| Field | Type | Description |
| :--- | :--- | :--- |
| `device_id`, `device_name`, `location` | `string` | Gateway identity configured in NVS |
| `seq` | `number` | Monotonic frame sequence number since boot |
| `timestamp_ms` | `number` | Device uptime timestamp in milliseconds |
| `bytes` | `number` | Raw UART bytes captured (`1..32768`) |
| `duration_ms` | `number` | Duration from first byte to last byte |
| `rate_bps` | `number` | Effective transfer rate (`bytes/sec`) |
| `esc_k_count` | `number` | Count of `1B 4B` (`ESC K`) commands detected |
| `raster_stripes` | `number` | Count of `1B 4B F0 00` 240-byte stripes detected |
| `reason` | `string` | `"idle_gap"`, `"max_bytes"`, or `"sim_test"` |
| `commands` | `object` | Presence flags for `1B40`, `1B4B`, `0D`, `0A`, `1B6C`, `1B31` |
| `hex` | `string` | Continuous uppercase hexadecimal string (`bytes * 2` chars) |

#### Mode `1`: Raw Uppercase HEX (`mqtt_format = 1`)
Plain uppercase hex string (`1B401B3100...`).

#### Mode `2`: Raw Binary (`mqtt_format = 2`)
Raw thermal printer byte stream (`uint8_t[]`).

### 2.3 Status & Heartbeat Payload (`topic_status`)

Published on MQTT connect and every `30s` (`retain = true`):

```json
{
  "status": "online",
  "device_id": "fi-gw-e011d8",
  "device_name": "FI-Inspection-Gateway",
  "location": "Inspection-Line-1",
  "ip": "172.16.166.59",
  "rssi": -53,
  "uptime_s": 95,
  "free_heap": 106336,
  "total_frames": 1,
  "total_rx_bytes": 10340,
  "mqtt_pub_ok": 1
}
```

Last Will and Testament (LWT) on unexpected disconnect (`retain = true`):

```json
{"status":"offline","device_id":"fi-gw-e011d8"}
```

### 2.4 Remote Commands (`topic_cmd`)

Publish any string containing one of these command tokens to `topic_cmd`:
* `"status"` or `"ping"` — Immediately publishes a fresh `online` status JSON to `topic_status`.
* `"test_uart"` — Injects a synthetic 4-stripe ESC/P test receipt through the capture & MQTT pipeline.
* `"reboot"` — Restarts the ESP32-C5 (`esp_restart()`).
* `"ota:<url>"` or `{"cmd":"ota","url":"http://..."}` — Triggers background HTTP/HTTPS OTA firmware update and reboots on verification.

---

## 3. ESC/P Raster HEX Decoding Specification (Backend & Frontend)

Every HMI thermal print receipt uses **240-column, 8-dot-high** raster stripes (`ESC K 240 0`):

1. **Strip whitespace** from the HEX string (`clean = hex.replace(/\s+/g, "")`).
2. **Extract 240-byte (`480` hex char) segments:**
   * Split on `"0D0A1B4BF000"` (`CR LF ESC K 0xF0 0x00`).
   * **Important (Stripe 0 preservation):** The very first stripe begins with `"1B401B31001B4BF000..."` (no preceding `0D0A`). Check `parts[0]` for `"1B4BF000"` and take the `480` hex chars immediately following it so the top receipt header (`检验记录表`) is not clipped.
   * For `parts[1..]`, take the first `480` hex chars (`part.slice(0, 480)`) whenever `part.length >= 480` (this safely ignores trailing printer commands like `0D0A1B3103` on the final stripe).
3. **Bitmap Dimensions:**
   * `width = 240` pixels
   * `height = segments.length * 8` pixels (typically `41` or `42` stripes = `328` or `336` px tall)
4. **Coordinate & Bit Polarity Mapping:**
   For stripe index `r` (`0..segments.length-1`), byte index `x` (`0..239`), bit index `b` (`0..7`):
   * `draw_x = 239 - x` *(byte 0 maps to the rightmost pixel column `239`)*
   * `draw_y = r * 8 + b` *(bit `0` is top row of the 8-pixel band, bit `7` is bottom row)*
   * `is_ink = (byte_val & (1 << b)) === 0` *(`0` bit = printed ink, `1` bit = blank paper)*

### Backend Reference (Python)

* Render to PNG: `hmi_to_pc/hmi_serial_to_pc.py` (`render_png_from_hex`) or `fabric_inspection_machine-main/fabric_inspection_machine-main/hmi_hex_to_image.py`
* Extract Inspection JSON (OCR): `fabric_inspection_machine-main/fabric_inspection_machine-main/hmi_hex_to_json_fast.py`

### Frontend Reference (JavaScript `<canvas>`)

See `drawReceipt(hexText)` in `main/web/index.html`:

```js
function decodeReceiptToCanvas(hexText, canvas, invert = false) {
  const clean = hexText.replace(/\s+/g, '');
  const ROW = 240, MARKER = '0D0A1B4BF000', FIRST = '1B4BF000';
  const parts = clean.split(MARKER);
  const segs = [];

  if (parts.length && parts[0].includes(FIRST)) {
    const tail = parts[0].split(FIRST)[1];
    if (tail && tail.length >= ROW * 2) segs.push(tail.slice(0, ROW * 2));
  }
  for (let i = 1; i < parts.length; i++) {
    if (parts[i].length >= ROW * 2) segs.push(parts[i].slice(0, ROW * 2));
  }
  if (!segs.length) return false;

  const w = ROW, h = segs.length * 8;
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(w, h);
  const d = img.data;

  for (let r = 0; r < segs.length; r++) {
    const s = segs[r];
    for (let x = 0; x < ROW; x++) {
      const byteVal = parseInt(s.substr(x * 2, 2), 16);
      const dx = ROW - 1 - x;
      for (let b = 0; b < 8; b++) {
        const ink = (byteVal & (1 << b)) === 0;
        const on = invert ? !ink : ink;
        const idx = ((r * 8 + b) * w + dx) * 4;
        d[idx] = on ? 240 : 12;
        d[idx + 1] = on ? 248 : 16;
        d[idx + 2] = on ? 255 : 24;
        d[idx + 3] = 255;
      }
    }
  }
  ctx.putImageData(img, 0, 0);
  return true;
}
```

---

## 4. Gateway HTTP REST API Reference

Base URL: `http://<gateway-ip>/` (SoftAP default: `http://192.168.4.1/`, LAN IP via DHCP).

### 4.1 Authentication

All `/api/*` endpoints except `/api/auth/status`, `/api/login`, and `/api/logout` require a valid session token:
* **Browser:** `HttpOnly` cookie `gw_sid=<32-hex-token>` (set automatically on `/api/login`).
* **API Clients / SPA:** Header `X-Auth-Token: <32-hex-token>`.
* Session TTL: `12 hours` (sliding on each authenticated call, up to `4` concurrent sessions).
* Unauthorized requests return HTTP `401` with `{"ok":false,"error":"Unauthorized"}`.

#### `GET /api/auth/status`
Public endpoint to check authentication state and device header info.
```json
{
  "authenticated": true,
  "user": "admin",
  "device_name": "FI-Inspection-Gateway",
  "device_id": "fi-gw-e011d8"
}
```

#### `POST /api/login`
Body:
```json
{"username": "admin", "password": "admin1234"}
```
Response (`200 OK`):
```json
{"ok": true, "token": "562abaddcd20db91f61a3146d135ef51", "user": "admin"}
```

#### `POST /api/logout`
Invalidates the active session token and clears `gw_sid`.

---

### 4.2 Monitoring & Captures

#### `GET /api/status`
Returns real-time device, Wi-Fi, MQTT, UART, and memory telemetry:
```json
{
  "uptime_s": 95,
  "free_heap": 106336,
  "min_heap": 44640,
  "device": {"name": "FI-Inspection-Gateway", "id": "fi-gw-e011d8", "location": "Inspection-Line-1"},
  "wifi": {
    "mode": 0, "sta_enabled": true, "sta_connected": true, "sta_ssid": "STLAN",
    "sta_ip": "172.16.166.59", "sta_gw": "172.16.160.254", "sta_mac": "D0:CF:13:E0:11:D8",
    "sta_rssi": -53, "ap_enabled": true, "ap_ssid": "FI-GW-E011D8",
    "ap_ip": "192.168.4.1", "ap_mac": "D0:CF:13:E0:11:D9", "ap_clients": 0, "last_err": ""
  },
  "mqtt": {
    "enabled": true, "connected": true, "host": "172.16.16.4", "port": 1883,
    "pub_ok": 1, "pub_err": 0, "reconnects": 0, "last_err": ""
  },
  "uart": {
    "baud": 9600, "tx_pin": 4, "rx_pin": 5, "data_bits": 8, "parity": 0, "stop_bits": 1,
    "idle_timeout_ms": 2000, "total_frames": 1, "total_rx_bytes": 10340, "uart_errors": 0
  }
}
```

#### `GET /api/captures`
Returns the last `8` capture metadata records (`history` array, newest first):
```json
{
  "history": [
    {
      "seq": 1,
      "timestamp_ms": 71841,
      "bytes": 10340,
      "duration_ms": 11760,
      "rate_bps": 879,
      "esc_k_count": 42,
      "raster_stripes": 42,
      "reason": "idle_gap",
      "mqtt_published": true,
      "commands": {"esc_at": true, "esc_k": true, "cr": true, "lf": true, "esc_l": false, "esc_1": true}
    }
  ]
}
```

#### `GET /api/captures/latest.hex` *(optional `?dl=1`)*
Streams the latest captured frame (`max 32768` raw bytes) as `text/plain` uppercase HEX wrapped at `80` characters per line. Passing `?dl=1` sets `Content-Disposition: attachment; filename="HMI_CAPTURE_latest.hex"`.

#### `GET /api/logs?since=<seq>`
Returns buffered ESP-IDF and gateway logs with `seq > since` (ring buffer holds last `80` lines):
```json
{
  "logs": [{"seq": 134, "ts": 72223, "text": "I (72223) rs232_rx: Frame #1 captured: ..."}],
  "last_seq": 134
}
```

#### `POST /api/logs/clear`
Clears the in-memory log ring buffer.

---

### 4.3 Configuration Endpoints

#### `GET /api/config`
Returns all non-secret configuration fields plus boolean flags `has_sta_pass`, `has_ap_pass`, and `has_mqtt_pass` (passwords are write-only).

#### `POST /api/config`
Accepts partial JSON updates. Only included keys are modified; empty password strings (`""`) preserve existing saved passwords unless `clear_sta_pass: true`, `clear_ap_pass: true`, or `clear_mqtt_pass: true` is sent.

| Section | Keys | Notes |
| :--- | :--- | :--- |
| Device | `device_name`, `device_id`, `location` | Saved to NVS |
| Auth | `admin_user`, `admin_pass` | Blank `admin_pass` preserves current password |
| Wi-Fi | `wifi_mode` (`0=AP+STA`, `1=STA`, `2=AP`), `sta_ssid`, `sta_pass`, `ap_ssid`, `ap_pass` | Reapplies Wi-Fi interface asynchronously (`350ms` delay) |
| UART1 | `uart_baud` (`1200..921600`), `uart_tx_pin`, `uart_rx_pin`, `uart_data_bits` (`7`/`8`), `uart_parity` (`0=N,1=O,2=E`), `uart_stop_bits` (`1`/`2`), `idle_timeout_ms` (`200..30000`), `max_capture_bytes` (`512..32768`), `auto_publish_mqtt`, `serial_echo_hex` | Hot-reconfigures `UART_NUM_1` immediately without reboot |
| MQTT | `mqtt_enabled`, `mqtt_host`, `mqtt_port`, `mqtt_client_id`, `mqtt_user`, `mqtt_pass`, `mqtt_keepalive`, `mqtt_qos` (`0..2`), `mqtt_retain`, `mqtt_format` (`0..2`), `mqtt_debug_stream`, `topic_hex`, `topic_status`, `topic_cmd`, `topic_debug` | Hot-reconnects MQTT client immediately |
| Debug | `verbose_debug` | Updates `esp_log_level_set` (`DEBUG` vs `INFO`) |

#### `POST /api/wifi/scan`
Runs an active Wi-Fi scan and returns:
```json
{
  "ok": true,
  "aps": [{"ssid": "STLAN", "rssi": -48, "channel": 1, "auth": 3}]
}
```

---

### 4.4 Control & Diagnostics Actions

| Method & URI | Action |
| :--- | :--- |
| `POST /api/actions/test-mqtt` | Publishes `{"type":"gateway_test",...}` to `topic_hex` |
| `POST /api/actions/test-uart` | Injects a synthetic 4-stripe ESC/P receipt into capture + MQTT |
| `POST /api/actions/reboot` | Responds `{"ok":true}` and reboots ESP32-C5 after `450ms` |
| `POST /api/actions/factory-reset` | Restores NVS defaults (`admin`/`admin1234`) and reboots |

---

### 4.5 OTA Firmware Update Endpoints

Dual-slot OTA partitions (`ota_0`, `ota_1`, `3M` each) with automatic rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

* **`GET /api/ota/status`** — Returns `state` (`"idle"`, `"downloading"`, `"uploading"`, `"verifying"`, `"rebooting"`, `"error"`), `running_partition`, `next_partition`, `app_version`, `compile_date`, `compile_time`, `pending_verify`, `bytes_written`, `total_bytes`, `last_error`.
* **`POST /api/ota/url`** — Body `{"url":"http://host/fi_hex_json.bin"}` (supports HTTP and HTTPS). Downloads, verifies, updates boot partition, and reboots.
* **`POST /api/ota/upload`** — Body raw `.bin` binary (`Content-Type: application/octet-stream`). Streams directly into passive OTA slot, validates image, sets boot partition, and reboots.

---

## 5. Frontend Integration Checklist

1. **Auth Flow:** Call `GET /api/auth/status` on startup. If `authenticated === false`, show login form and `POST /api/login`. Store `token` in `sessionStorage` and send `X-Auth-Token` (in addition to `credentials: 'same-origin'` for cookies).
2. **Polling Loop (`2s`):** Poll `GET /api/status` and `GET /api/logs?since=${lastLogSeq}`. Track `status.uart.total_frames` — whenever it changes, fetch `GET /api/captures` and `GET /api/captures/latest.hex` and re-render the `<canvas>` receipt preview.
3. **Frontend Build & Embedding:** The UI lives in `main/web/index.html` and is embedded directly into flash via `EMBED_TXTFILES "web/index.html"` in `main/CMakeLists.txt` (zero external CDN requests, works offline on SoftAP `192.168.4.1`). Any edit to `main/web/index.html` only requires rebuilding and flashing `fi_hex_json.bin`.
