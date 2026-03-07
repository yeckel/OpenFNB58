# OpenFNB58

Open-source tools for the **FNIRSI FNB58** USB power meter / charger tester.

Includes a Python CLI reader and a Qt 6 / QML desktop application with live charts,
range measurement, and CSV/Excel export.

---

## Features

### Python CLI (`fnb58.py`)
- Reads live measurements over **USB HID** (`/dev/hidraw*`) or **Bluetooth LE**
- Reports VBUS, IBUS, Power, D+, D−, Temperature
- No root required — uses `/dev/hidraw*` (world-accessible) and unprivileged D-Bus BLE
- Modes: single shot, continuous, with configurable interval

### Qt/QML GUI (`app/`)
- **Live scrolling charts** — Voltage/Current (dual axis) + Power (area fill)
- **Live readout bar** — VBUS, IBUS, Power, D+, D−, Temperature
- **USB and BLE transport** — auto-discovery for USB; scan + pick for BLE
- **Range measurement** — click-drag on any chart to select a time window; shows energy (Wh/mWh/mAh), mean & peak V/I/P, temperature min/max
- **Energy marker** — reset the Wh accumulator mid-session to measure a charging phase
- **Export** — native file dialog, CSV and Excel (.xlsx) output
- Dark Material theme, configurable time window, follow mode

---

## Hardware

| Property | Value |
|----------|-------|
| Device   | FNIRSI FNB58 |
| USB VID:PID | `2E3C:5558` |
| USB interface | HID (interface #3) → `/dev/hidraw*` |
| BLE service | `ffe0` (RFstar transparent UART) |
| BLE notify char | `ffe4` |
| BLE write char  | `ffe9` |

---

## Requirements

### Python CLI
- Python 3.8+
- `python3-dbus`, `python3-gi` (for BLE mode; system packages)
- User must be in the `plugdev` group (for `/dev/hidraw*` access)

```bash
sudo usermod -aG plugdev $USER   # re-login after
```

### Qt GUI
- **Qt 6.5+** with: Core, Gui, Quick, QuickControls2, QML, DBus, Widgets, QuickDialogs2
- CMake 3.28+, GCC with C++20

Tested with Qt 6.9.2 from the [Qt online installer](https://www.qt.io/download).

```bash
# Install Qt (aqt example)
pip install aqtinstall
aqt install-qt linux desktop 6.9.2 gcc_64
```

For BLE, BlueZ must be running and the device pre-paired:
```bash
bluetoothctl
  scan on
  pair BA:03:18:7A:23:DF
  trust BA:03:18:7A:23:DF
```

---

## USB HID Protocol

The FNB58 uses 64-byte HID reports over `/dev/hidraw*`.

| Command | Bytes |
|---------|-------|
| INIT1   | `AA 81 00 … 8E` |
| INIT2   | `AA 82 00 … 96` |
| POLL    | `AA 83 00 … 9E` |

Response header: `AA 04`, followed by 4 × 15-byte samples.

| Field    | Offset | Type        | Scale     |
|----------|--------|-------------|-----------|
| Voltage  | 0      | uint32-LE   | ÷ 100000  |
| Current  | 4      | uint32-LE   | ÷ 100000  |
| D+       | 8      | uint16-LE   | ÷ 1000    |
| D−       | 10     | uint16-LE   | ÷ 1000    |
| Temp     | 12     | uint16-LE   | ÷ 10      |

> **Important:** sending INIT1 (`AA 81`) to a device that is already streaming causes
> `ETIMEDOUT` and a USB disconnect. The code tries POLL first, falls back to INIT2,
> and only sends INIT1 as a last resort.

## BLE Protocol

Stream frames on `ffe4`: `AA [type] [data_len] [data…] [checksum]`

| Type | Data | Content |
|------|------|---------|
| `07` | 4 B  | VBUS uint16-LE ÷ 1000, IBUS uint16-LE ÷ 1000 |
| `06` | 6 B  | D+ uint16-LE ÷ 1000, D− uint16-LE ÷ 1000 |

Initialisation sequence sent to `ffe9`:
1. `AA 81 00 F4` — wake
2. wait ~2 s
3. `AA 82 00 A7` — start stream

---

## Usage

### Python CLI

```bash
# Single reading (USB)
python3 fnb58.py --once

# Continuous USB monitoring (1 s interval)
python3 fnb58.py --usb --interval 1

# BLE monitoring (auto-detect MAC)
python3 fnb58.py --ble

# BLE with explicit MAC
python3 fnb58.py --ble --mac BA:03:18:7A:23:DF
```

### Qt GUI

```bash
cd app
bash build.sh                      # configure + build (Qt 6.9 in /opt/Qt)
./build/bin/fnb58app
```

**USB mode:** select *USB*, leave Device/MAC blank, click ▶ Start.  
**BLE mode:** select *BLE*, click ⊕ Scan, pick your device, click ▶ Start.

#### Measurement
Click and drag on any chart to select a time range.  
A panel slides up showing energy, mean/peak voltage, current and power for the selection.  
Double-click or press **✕ Clear** to dismiss.

---

## Project Structure

```
OpenFNB58/
├── fnb58.py                  Python CLI (USB HID + BLE)
└── app/
    ├── CMakeLists.txt
    ├── build.sh
    ├── main.cpp
    ├── DataRecord.h           Measurement sample struct
    ├── BaseTransport.h        Abstract QThread transport
    ├── UsbTransport.h/cpp     USB HID worker
    ├── BleTransport.h/cpp     BLE / BlueZ D-Bus worker
    ├── DeviceBackend.h/cpp    QML-exposed backend (energy, export, measure)
    ├── XlsxWriter.h/cpp       OOXML .xlsx writer (no external deps)
    ├── ZipWriter.h/cpp        STORE-only ZIP (used by XlsxWriter)
    └── qml/
        ├── Main.qml           Application window, toolbar, stats panel
        └── LiveChart.qml      Canvas-based scrolling dual-axis chart
```

---

## Acknowledgements

- Protocol reverse-engineered with help from
  [baryluk/fnirsi-usb-power-data-logger](https://github.com/baryluk/fnirsi-usb-power-data-logger)
  and [Boondock-Echo/Article-Files](https://github.com/Boondock-Echo/Article-Files/tree/main/FNIRSI-FNB58).

## License

MIT
