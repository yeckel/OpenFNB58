#!/usr/bin/env python3
"""
FNB58 USB Power Meter reader

Supports two transport modes:
  --usb  (default) USB HID via pyusb (interface 3, endpoints 0x03/0x83)
  --ble            Bluetooth LE via BlueZ D-Bus (ffe0 GATT service)

=== USB HID protocol (64-byte reports) ===
  Init: aa 81 00..00 8e  / aa 82 00..00 96 (×2 for FNB58)
  Poll: aa 83 00..00 9e  (every ~1 s)
  Data packet [0]=0xaa [1]=0x04, then 4×15-byte samples:
    [0..3]   voltage  uint32-LE / 100000  V
    [4..7]   current  uint32-LE / 100000  A
    [8..9]   D+       uint16-LE / 1000    V
    [10..11] D-       uint16-LE / 1000    V
    [12]     unknown
    [13..14] temp     uint16-LE / 10      °C

=== BLE protocol (GATT, service ffe0, RFstar transparent UART) ===
  Device: FNB58-XXXXXX  (default MAC set by BLE_DEFAULT_MAC below)
  Char ffe9 (write / write-without-response): send commands
  Char ffe4 (notify): receive streaming data

  Init sequence (4-byte commands sent to ffe9):
    aa 81 00 f4   — wake up / enter measurement mode
    aa 82 00 a7   — start data stream  (send ~2 s after first command)

  Stream packet framing:  aa [type] [data_len] [data...] [checksum]
    total bytes = data_len + 4

  Packet types in the stream:
    0x07  VBUS/IBUS — data_len=4
            data[0..1] uint16-LE / 1000  VBUS (V)
            data[2..3] uint16-LE / 1000  IBUS (A)
    0x06  D+/D-     — data_len=6
            data[0..1] uint16-LE / 1000  D+ (V)
            data[2..3] uint16-LE / 1000  D- (V)
            data[4..5] unknown
    0x04  Accumulated — data_len=12
            data[0..3] uint32-LE  accumulated charge (unit TBD)
    0x03  Device info — data_len=14  (sent once on init)
    0x05  Status/misc — data_len=7
    0x08  Config/misc — data_len=17

  Multiple packet types may be concatenated in a single BLE notification chunk.
"""

import sys
import os
import errno
import glob
import select
import subprocess
import time
import struct
import argparse

VID_FNB58 = "2e3c"
PID_FNB58 = "5558"

SAMPLES_PER_SECOND = 100
TIME_INTERVAL = 1.0 / SAMPLES_PER_SECOND

CMD_INIT1  = b"\xaa\x81" + b"\x00" * 61 + b"\x8e"
CMD_INIT2  = b"\xaa\x82" + b"\x00" * 61 + b"\x96"
CMD_POLL   = b"\xaa\x83" + b"\x00" * 61 + b"\x9e"

# ── Protocol definitions ──────────────────────────────────────────────────
# Matching the FNIRSI Windows app's protocol list.
# Protocol IDs are used by the trigger command (cmd 0x85).
# NOTE: The trigger command byte (0x85) and payload format are derived from
#       reverse engineering; verify with a QC/PD-capable charger.
PROTOCOLS = {
    # id: (name, default_voltage_mv, [available_mv])
     1: ("QC 2.0 5V",    5000,  [5000]),
     2: ("QC 2.0 9V",    9000,  [9000]),
     3: ("QC 2.0 12V",  12000,  [12000]),
     4: ("QC 2.0 20V",  20000,  [20000]),
     5: ("QC 3.0",       9000,  list(range(3600, 20001, 200))),
     6: ("FCP/AFC 9V",   9000,  [9000]),
     7: ("FCP/AFC 12V", 12000,  [12000]),
     8: ("Huawei FCP",   9000,  [9000, 12000]),
     9: ("Huawei SCP",   5000,  list(range(4000, 8001, 100))),
    10: ("Samsung 2A",   5000,  [5000]),
    11: ("Samsung AFC",  9000,  [9000, 12000]),
    12: ("Apple 2.1A",   5000,  [5000]),
    13: ("Apple 2.4A",   5000,  [5000]),
    14: ("PD/MTK",       9000,  [5000, 9000, 12000, 15000, 20000]),
}

CMD_TRIGGER_BYTE = 0x85   # single trigger (best-guess; not officially documented)
CMD_RELEASE_BYTE = 0x86   # release trigger (best-guess)


def crc8_fnb58(data: bytes) -> int:
    """
    CRC-8 over the given bytes.
    Parameters: poly=0x39, init=0x42, refin=False, refout=False, xorout=0x00.
    For a 64-byte USB HID frame, pass bytes[1:63].
    """
    poly, crc = 0x39, 0x42
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def make_hid_cmd(cmd_byte: int, payload: bytes = b"") -> bytes:
    """Build a 64-byte USB HID command with the correct CRC-8 checksum."""
    inner = bytes([cmd_byte]) + bytes(payload) + bytes(61 - len(payload))
    return bytes([0xAA]) + inner + bytes([crc8_fnb58(inner)])


def make_trigger_cmd(proto_id: int, voltage_mv: int = 0) -> bytes:
    """
    Build a trigger command for the given protocol ID and target voltage.
    voltage_mv is encoded as a LE uint16 in 10 mV units (e.g. 9000 mV → 0x0384).
    """
    v10 = voltage_mv // 10  # 10 mV resolution
    payload = bytes([proto_id, v10 & 0xFF, (v10 >> 8) & 0xFF])
    return make_hid_cmd(CMD_TRIGGER_BYTE, payload)


def make_release_cmd() -> bytes:
    """Build the trigger release command."""
    return make_hid_cmd(CMD_RELEASE_BYTE)


def find_hidraw(wait: float = 10.0) -> str:
    """
    Find the /dev/hidrawN node for the FNB58 by scanning sysfs.
    Waits up to `wait` seconds for the device to appear (it may be rebooting).
    Returns the device path, or exits with an error if not found.
    """
    deadline = time.time() + wait
    while True:
        for hidraw_dir in glob.glob("/sys/class/hidraw/hidraw*"):
            uevent_path = os.path.join(hidraw_dir, "device", "..", "uevent")
            try:
                uevent = open(uevent_path).read()
            except OSError:
                continue
            product = ""
            for line in uevent.splitlines():
                if line.startswith("PRODUCT="):
                    product = line.split("=", 1)[1].lower()
            if product.startswith(f"{VID_FNB58}/{PID_FNB58}/"):
                return f"/dev/{os.path.basename(hidraw_dir)}"

        if time.time() >= deadline:
            break
        print("Waiting for FNB58 HID device...", file=sys.stderr)
        time.sleep(1.0)

    print(
        "FNB58 HID device not found after waiting. Is it connected?",
        file=sys.stderr,
    )
    sys.exit(1)


def open_hid(path: str) -> int:
    """Open the hidraw device; returns a file descriptor, or exits on error."""
    try:
        return os.open(path, os.O_RDWR)
    except PermissionError:
        print(f"Cannot open {path}: permission denied.", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print(f"Cannot open {path}: {e}", file=sys.stderr)
        sys.exit(1)


def hid_write(fd: int, cmd: bytes, retries: int = 5) -> bool:
    """
    Write a HID command.  Retries on ETIMEDOUT (device busy streaming).
    Returns True on success, False on hard failure.
    """
    for _ in range(retries):
        try:
            os.write(fd, cmd)
            return True
        except OSError as e:
            if e.errno in (errno.ETIMEDOUT, errno.EPROTO, errno.ENODEV):
                time.sleep(0.15)
                continue
            return False
    return False


def hid_drain(fd: int, max_packets: int = 200):
    """
    Drain pending HID input reports so the device isn't backlogged when we write.
    Stops when no packet arrives within 20 ms or max_packets reached.
    """
    for _ in range(max_packets):
        r, _, _ = select.select([fd], [], [], 0.02)
        if not r:
            break
        try:
            os.read(fd, 64)
        except OSError:
            break


def infer_protocol(dp: float, dn: float, vbus: float = 5.0) -> str:
    """
    Infer the charging protocol from D+/D- voltages measured by the FNB58.
    These voltages reflect what the charger (upstream side) is asserting.

    Returns a short protocol name string.
    """
    tol = 0.18  # ±180 mV tolerance

    def near(v, target):
        return abs(v - target) < tol

    def near2(v1, t1, v2, t2):
        return near(v1, t1) and near(v2, t2)

    # No voltage on either line → SDP or no charger detected
    if dp < 0.35 and dn < 0.35:
        return "USB SDP"

    # Both lines equal and low → BC 1.2 DCP / Samsung-style charger
    if near(dp, dn) and 0.5 < dp < 2.5:
        if near2(dp, 2.0, dn, 2.0):
            return "Apple 1A"
        if near2(dp, 2.7, dn, 2.0):
            return "Apple 2.1A"
        if near2(dp, 2.0, dn, 2.7):
            return "Apple 2.4A"
        if 1.0 < dp < 1.8:
            return "DCP / Samsung"
        if 1.8 < dp < 2.3:
            return "Apple 1A"
        return f"DCP ({dp:.2f}V)"

    # QC 2.0 voltage codes (D+, D-)
    if near2(dp, 0.6, dn, 0.0):
        return "QC 2.0 5V"
    if near2(dp, 3.3, dn, 0.6):
        return "QC 2.0 9V"
    if near2(dp, 0.6, dn, 0.6):
        return "QC 2.0 12V / CDP"
    if near2(dp, 3.3, dn, 3.3):
        return "QC 2.0 20V"
    # QC 3.0 continuous mode: D+ = 0.6 V, D- toggling
    if near(dp, 0.6) and 0.0 < dn < 3.6:
        return "QC 3.0"

    # Huawei FCP: D+ ≈ 0.325 V used as handshake initiation signal
    if 0.2 < dp < 0.45 and dn < 0.3:
        return "Huawei FCP"

    # If VBUS is elevated but D+/D- still show 5V pattern → USB PD (CC pins used)
    if vbus > 5.5 and dp < 0.5 and dn < 0.5:
        return f"USB PD ({vbus:.0f}V)"
    if vbus > 5.5:
        return f"Fast Charge ({vbus:.0f}V)"

    return f"Unknown (D+={dp:.2f}V D-={dn:.2f}V)"


def decode_packet(data):
    """Decode a 64-byte data packet into a list of sample dicts."""
    if data[0] != 0xAA or data[1] != 0x04:
        return None  # not a data packet

    samples = []
    for i in range(4):
        off = 2 + 15 * i
        voltage = struct.unpack_from("<I", data, off)[0] / 100000.0
        current = struct.unpack_from("<I", data, off + 4)[0] / 100000.0
        dp      = struct.unpack_from("<H", data, off + 8)[0] / 1000.0
        dn      = struct.unpack_from("<H", data, off + 10)[0] / 1000.0
        temp    = struct.unpack_from("<H", data, off + 13)[0] / 10.0
        power   = voltage * current
        samples.append({
            "voltage_V": voltage,
            "current_A": current,
            "power_W":   power,
            "dp_V":      dp,
            "dn_V":      dn,
            "temp_C":    temp,
            "protocol":  infer_protocol(dp, dn, voltage),
        })
    return samples


def hid_read(fd: int, timeout: float = 2.0) -> bytes | None:
    """Read one 64-byte HID report with a timeout. Returns None on timeout."""
    r, _, _ = select.select([fd], [], [], timeout)
    if not r:
        return None
    try:
        return os.read(fd, 64)
    except OSError:
        return None


def _hid_init(fd: int) -> bool:
    """
    Drain any pending data, then send the init sequence.
    Returns True if init commands were written successfully.
    """
    hid_drain(fd)
    if not hid_write(fd, CMD_INIT1):
        return False
    time.sleep(0.05)
    if not hid_write(fd, CMD_INIT2):
        return False
    time.sleep(0.05)
    if not hid_write(fd, CMD_INIT2):
        return False
    return True


def read_once(fd: int) -> list | None:
    """
    Read one measurement.  Tries progressively more aggressive init sequences:
      1. Drain + poll only  (device already streaming from a prior run)
      2. Drain + INIT2×2   (gentle restart, avoids the aa81 reboot risk)
      3. Drain + full INIT  (aa81 + aa82×2, only if gentler methods fail)
    """
    for phase, cmds in enumerate([
        [CMD_POLL],
        [CMD_INIT2, CMD_INIT2],
        [CMD_INIT1, CMD_INIT2, CMD_INIT2],
    ]):
        hid_drain(fd)
        for cmd in cmds:
            if not hid_write(fd, cmd):
                return None
            time.sleep(0.05)

        deadline = time.time() + (1.5 if phase == 0 else 4.0)
        while time.time() < deadline:
            data = hid_read(fd, timeout=min(1.5, deadline - time.time()))
            if data is None:
                break
            samples = decode_packet(data)
            if samples:
                return samples

    return None


def run_monitor(fd: int, interval=1.0, count=None):
    """Continuously poll and print measurements."""
    hid_drain(fd)
    if not hid_write(fd, CMD_INIT1) or not hid_write(fd, CMD_INIT2) or not hid_write(fd, CMD_INIT2):
        print("Failed to initialize device.", file=sys.stderr)
        sys.exit(1)

    print(f"{'Time':>10}  {'Voltage':>9}  {'Current':>9}  {'Power':>8}  {'D+':>6}  {'D-':>6}  {'Temp':>7}  Protocol")
    print(f"{'(s)':>10}  {'(V)':>9}  {'(A)':>9}  {'(W)':>8}  {'(V)':>6}  {'(V)':>6}  {'(°C)':>7}")
    print("-" * 90)

    next_poll = time.time() + interval
    start = time.time()
    n = 0

    try:
        while count is None or n < count:
            now = time.time()
            if now >= next_poll:
                hid_write(fd, CMD_POLL)
                next_poll = now + interval

            data = hid_read(fd, timeout=min(1.0, max(0.05, next_poll - time.time())))
            if data is None:
                continue

            samples = decode_packet(data)
            if samples is None:
                continue

            t = time.time() - start
            s = samples[0]
            print(
                f"{t:10.2f}  {s['voltage_V']:9.5f}  {s['current_A']:9.5f}"
                f"  {s['power_W']:8.5f}  {s['dp_V']:6.3f}  {s['dn_V']:6.3f}  {s['temp_C']:7.1f}"
                f"  {s['protocol']}"
            )
            sys.stdout.flush()
            n += 1

    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)


def trigger(fd: int, proto_id: int, voltage_mv: int = 0, hold_secs: float = 3.0):
    """
    Send a single trigger command to the FNB58 to negotiate a fast-charge protocol.

    proto_id    : protocol ID (1–14, see PROTOCOLS dict)
    voltage_mv  : target voltage in millivolts (0 = use protocol default)
    hold_secs   : seconds to hold the trigger before releasing (0 = no release)

    NOTE: Trigger command bytes are reverse-engineered / best-guess.
          Test with a QC/PD-capable charger and report results.
    """
    proto = PROTOCOLS.get(proto_id)
    if proto is None:
        print(f"Unknown protocol ID {proto_id}. Valid IDs: {list(PROTOCOLS.keys())}", file=sys.stderr)
        sys.exit(1)

    name, default_mv, _ = proto
    if voltage_mv == 0:
        voltage_mv = default_mv

    print(f"Triggering: {name} @ {voltage_mv/1000:.1f} V (proto_id={proto_id})", file=sys.stderr)
    hid_write(fd, make_trigger_cmd(proto_id, voltage_mv))

    if hold_secs > 0:
        time.sleep(hold_secs)
        hid_write(fd, make_release_cmd())
        print("Trigger released.", file=sys.stderr)


# ──────────────────────────────────────────────────────────────────────────────
# BLE transport (BlueZ D-Bus, service ffe0 / RFstar transparent UART)
# ──────────────────────────────────────────────────────────────────────────────

BLE_DEFAULT_MAC = "BA:03:18:7A:23:DF"

# UUIDs for the transparent UART characteristics
UUID_NOTIFY = "0000ffe4-0000-1000-8000-00805f9b34fb"  # ffe4 — notifications
UUID_WRITE  = "0000ffe9-0000-1000-8000-00805f9b34fb"  # ffe9 — write

# 4-byte init commands sent over ffe9 to start the measurement stream
BLE_CMD_INIT1 = bytes([0xaa, 0x81, 0x00, 0xf4])
BLE_CMD_INIT2 = bytes([0xaa, 0x82, 0x00, 0xa7])

# Stream packet data_len by type (used for framing)
_BLE_PKT_LENS = {0x03: 14, 0x04: 12, 0x05: 7, 0x06: 6, 0x07: 4, 0x08: 17}


def _ble_ensure_connected(mac: str):
    """Connect to BLE device via bluetoothctl if not already connected."""
    import dbus
    try:
        bus = dbus.SystemBus()
        dev_path = f"/org/bluez/hci0/dev_{mac.replace(':', '_').upper()}"
        props = dbus.Interface(bus.get_object("org.bluez", dev_path), "org.freedesktop.DBus.Properties")
        if props.Get("org.bluez.Device1", "Connected"):
            return  # already connected
    except Exception:
        pass

    print(f"Connecting to {mac}...", file=sys.stderr)
    result = subprocess.run(
        ["bluetoothctl", "connect", mac],
        capture_output=True, text=True, timeout=15
    )
    if "Connection successful" not in result.stdout:
        print(f"Could not connect: {result.stdout.strip()}", file=sys.stderr)
        sys.exit(1)
    time.sleep(1.5)  # give BlueZ time to resolve GATT services


def _ble_find_chars(bus, mac: str) -> tuple[str, str]:
    """
    Enumerate BlueZ GATT objects to find the D-Bus paths for ffe4 and ffe9
    characteristics under the given device MAC. Returns (notify_path, write_path).
    """
    import dbus
    mgr = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
    objs = mgr.GetManagedObjects()
    dev_prefix = f"/org/bluez/hci0/dev_{mac.replace(':', '_').upper()}"
    notify_path = write_path = None
    for path, ifaces in objs.items():
        if not str(path).startswith(dev_prefix):
            continue
        char = ifaces.get("org.bluez.GattCharacteristic1")
        if not char:
            continue
        uuid = str(char.get("UUID", ""))
        if uuid == UUID_NOTIFY:
            notify_path = str(path)
        elif uuid == UUID_WRITE:
            write_path = str(path)
    if not notify_path or not write_path:
        print(
            f"Could not find ffe4/ffe9 characteristics for {mac}.\n"
            "Is the device connected?  Try: bluetoothctl connect " + mac,
            file=sys.stderr,
        )
        sys.exit(1)
    return notify_path, write_path


def _parse_ble_stream(chunk: bytes) -> list[dict]:
    """
    Parse a BLE notification chunk that may contain multiple concatenated packets.
    Each packet: aa [type] [data_len] [data...] [checksum]
    Returns a list of decoded measurement dicts (only types 0x06 and 0x07).
    """
    results = []
    i = 0
    while i < len(chunk):
        if chunk[i] != 0xAA:
            i += 1
            continue
        if i + 2 >= len(chunk):
            break
        ptype = chunk[i + 1]
        dlen  = chunk[i + 2]
        expected = _BLE_PKT_LENS.get(ptype)
        if expected is None or dlen != expected:
            i += 1
            continue
        total = dlen + 4  # aa + type + len + data + checksum
        if i + total > len(chunk):
            break
        data = chunk[i + 3 : i + 3 + dlen]
        if ptype == 0x07:
            vbus = struct.unpack_from("<H", data, 0)[0] / 1000.0
            ibus = struct.unpack_from("<H", data, 2)[0] / 1000.0
            results.append({"type": "vi", "vbus_V": vbus, "ibus_A": ibus, "power_W": vbus * ibus})
        elif ptype == 0x06:
            dp = struct.unpack_from("<H", data, 0)[0] / 1000.0
            dn = struct.unpack_from("<H", data, 2)[0] / 1000.0
            results.append({"type": "dp", "dp_V": dp, "dn_V": dn})
        i += total
    return results


def _ble_require_imports():
    try:
        import dbus
        import dbus.mainloop.glib
        from gi.repository import GLib
        return dbus, GLib
    except ImportError:
        print("BLE mode requires: sudo apt install python3-dbus python3-gi", file=sys.stderr)
        sys.exit(1)


def ble_read_once(mac: str) -> dict | None:
    """
    Start the BLE stream, collect the first VBUS/IBUS + D+/D- readings, return them.
    """
    dbus, GLib = _ble_require_imports()
    import dbus.mainloop.glib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    _ble_ensure_connected(mac)
    bus = dbus.SystemBus()

    notify_path, write_path = _ble_find_chars(bus, mac)
    ni = dbus.Interface(bus.get_object("org.bluez", notify_path), "org.bluez.GattCharacteristic1")
    wi = dbus.Interface(bus.get_object("org.bluez", write_path),  "org.bluez.GattCharacteristic1")

    result = {}
    loop = GLib.MainLoop()

    def on_data(iface, changed, inv):
        if "Value" not in changed:
            return
        for pkt in _parse_ble_stream(bytes(changed["Value"])):
            result.update(pkt)
        if "vbus_V" in result and "dp_V" in result:
            loop.quit()

    bus.add_signal_receiver(on_data,
        dbus_interface="org.freedesktop.DBus.Properties",
        signal_name="PropertiesChanged", path=notify_path)

    try: ni.StopNotify()
    except Exception: pass
    ni.StartNotify()

    def _send_init2():
        wi.WriteValue(dbus.Array([dbus.Byte(b) for b in BLE_CMD_INIT2], signature="y"), {})
    wi.WriteValue(dbus.Array([dbus.Byte(b) for b in BLE_CMD_INIT1], signature="y"), {})
    GLib.timeout_add(2000, lambda: _send_init2() or False)
    GLib.timeout_add(8000, lambda: loop.quit() or False)
    loop.run()

    try: ni.StopNotify()
    except Exception: pass

    return result if "vbus_V" in result else None


def ble_monitor(mac: str, count: int | None = None):
    """Stream measurements continuously from BLE and print each VBUS/IBUS packet."""
    dbus, GLib = _ble_require_imports()
    import dbus.mainloop.glib
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    _ble_ensure_connected(mac)
    bus = dbus.SystemBus()

    notify_path, write_path = _ble_find_chars(bus, mac)
    ni = dbus.Interface(bus.get_object("org.bluez", notify_path), "org.bluez.GattCharacteristic1")
    wi = dbus.Interface(bus.get_object("org.bluez", write_path),  "org.bluez.GattCharacteristic1")

    print(f"BLE mode — device {mac}")
    print(f"{'Time':>10}  {'VBUS':>9}  {'IBUS':>9}  {'Power':>9}  {'D+':>7}  {'D-':>7}")
    print(f"{'(s)':>10}  {'(V)':>9}  {'(A)':>9}  {'(W)':>9}  {'(V)':>7}  {'(V)':>7}")
    print("-" * 68)

    start = time.time()
    n = [0]
    last_dp = [None]
    loop = GLib.MainLoop()

    def on_data(iface, changed, inv):
        if "Value" not in changed:
            return
        for pkt in _parse_ble_stream(bytes(changed["Value"])):
            if pkt["type"] == "dp":
                last_dp[0] = pkt
            elif pkt["type"] == "vi":
                t = time.time() - start
                dp = last_dp[0]["dp_V"] if last_dp[0] else float("nan")
                dn = last_dp[0]["dn_V"] if last_dp[0] else float("nan")
                print(
                    f"{t:10.2f}  {pkt['vbus_V']:9.3f}  {pkt['ibus_A']:9.3f}"
                    f"  {pkt['power_W']:9.5f}  {dp:7.3f}  {dn:7.3f}"
                )
                sys.stdout.flush()
                n[0] += 1
                if count is not None and n[0] >= count:
                    loop.quit()

    bus.add_signal_receiver(on_data,
        dbus_interface="org.freedesktop.DBus.Properties",
        signal_name="PropertiesChanged", path=notify_path)

    try: ni.StopNotify()
    except Exception: pass
    ni.StartNotify()

    def _send_init2():
        wi.WriteValue(dbus.Array([dbus.Byte(b) for b in BLE_CMD_INIT2], signature="y"), {})
    wi.WriteValue(dbus.Array([dbus.Byte(b) for b in BLE_CMD_INIT1], signature="y"), {})
    GLib.timeout_add(2000, lambda: _send_init2() or False)

    try:
        loop.run()
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)

    try: ni.StopNotify()
    except Exception: pass


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Read VBUS/IBUS and more from an FNIRSI FNB58 USB power meter.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument(
        "--usb", action="store_true", default=True,
        help="Use USB HID (default)"
    )
    transport.add_argument(
        "--ble", action="store_true",
        help="Use Bluetooth LE (requires python3-dbus, python3-gi)"
    )
    parser.add_argument(
        "--mac", default=BLE_DEFAULT_MAC,
        help="BLE device MAC address"
    )
    parser.add_argument(
        "-n", "--count", type=int, default=None,
        help="Number of readings (default: run forever, Ctrl-C to stop)"
    )
    parser.add_argument(
        "--once", action="store_true",
        help="Take a single snapshot and exit"
    )
    parser.add_argument(
        "--interval", type=float, default=1.0,
        help="Poll interval in seconds"
    )
    parser.add_argument(
        "--trigger", type=int, default=None, metavar="PROTO_ID",
        help=(
            "Send a fast-charge trigger and release it after --hold seconds. "
            "PROTO_ID: " + ", ".join(f"{k}={v[0]}" for k,v in PROTOCOLS.items())
        )
    )
    parser.add_argument(
        "--voltage", type=float, default=0,
        help="Trigger target voltage in V (0 = protocol default)"
    )
    parser.add_argument(
        "--hold", type=float, default=3.0,
        help="Seconds to hold the trigger before releasing (0 = no release)"
    )
    args = parser.parse_args()

    # ── BLE mode ──
    if args.ble:
        if args.once:
            s = ble_read_once(args.mac)
            if s:
                dp  = s.get('dp_V', float('nan'))
                dn  = s.get('dn_V', float('nan'))
                vbs = s.get('vbus_V', float('nan'))
                print(f"VBUS     : {vbs:.3f} V")
                print(f"IBUS     : {s.get('ibus_A', float('nan')):.3f} A")
                print(f"Power    : {s.get('power_W', float('nan')):.5f} W")
                print(f"D+       : {dp:.3f} V")
                print(f"D-       : {dn:.3f} V")
                print(f"Protocol : {infer_protocol(dp, dn, vbs)}")
            else:
                print("No BLE data received.", file=sys.stderr)
                sys.exit(1)
        else:
            ble_monitor(args.mac, count=args.count)
        return

    # ── USB HID mode ──
    path = find_hidraw()
    fd = open_hid(path)

    try:
        if args.trigger is not None:
            # Ensure device is streaming first
            read_once(fd)
            trigger(fd, args.trigger, int(args.voltage * 1000), args.hold)
        elif args.once:
            samples = read_once(fd)
            if samples is None:
                # Device may be mid-reboot; wait for it to come back and retry
                os.close(fd)
                path = find_hidraw(wait=20.0)
                fd = open_hid(path)
                samples = read_once(fd)
            if samples:
                s = samples[0]
                print(f"VBUS     : {s['voltage_V']:.5f} V")
                print(f"IBUS     : {s['current_A']:.5f} A")
                print(f"Power    : {s['power_W']:.5f} W")
                print(f"D+       : {s['dp_V']:.3f} V")
                print(f"D-       : {s['dn_V']:.3f} V")
                print(f"Temp     : {s['temp_C']:.1f} °C")
                print(f"Protocol : {s['protocol']}")
            else:
                print("No data received.", file=sys.stderr)
                sys.exit(1)
        else:
            run_monitor(fd, interval=args.interval, count=args.count)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
