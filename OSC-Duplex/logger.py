"""
logger.py — ESP32 OSC benchmark logger
==================================================

SUMMARY
-------
Receives UDP packets from the ESP32 benchmark sender, parses both RAW and OSC
payloads, and combines them with serial-side diagnostics from the device.

WHAT IT TRACKS
--------------
  send cost         Time the ESP32 spent in the previous send call
                    (prev_send_cost_us embedded in each packet).

  send interval     Start-to-start spacing between sends
                    (derived from send_start_us).

  receive interval  Host-side arrival spacing between packets.

  receive loss      Missing sequence numbers, or missing addr x tick deliveries
                    for multi-address modes.

  ESP send errors   E rows from serial (build/send failures on the device).
  ESP summaries     S/T rows from serial (attempted / ok / elapsed).
  WiFi context      N/W rows from serial (run snapshots and event log).

HOW IT WORKS
------------
1. A UDP socket listens for benchmark packets.
2. Each packet is parsed as RAW, OSC, or OSC bundle.
3. Packet `run_id` is joined with serial metadata to recover transport/mode.
4. A background serial thread captures ESP-side serial output.
5. Each packet is written to CSV.

OUTPUTS
-------
    benchmark_packets.csv   one row per parsed message:
                            one row for a normal packet, or one row per
                            submessage inside an OSC bundle

                            receiver_rx_ns     host receive timestamp
                                              Linux: kernel socket timestamp
                                              others: time.time_ns() after receive
                            run_id             benchmark run id from the packet
                            transport/mode     recovered from serial metadata
                            seq                packet sequence number
                            send_start_us      ESP send start timestamp
                            prev_send_cost_us  ESP time spent in the previous send
                            parse_* fields     whether parsing worked, and why not
                            ramp_* fields      extra ramp settings when available
    serial_log.txt          raw serial output from the ESP32

USAGE
-----
  python3 logger.py
  ESP32_SERIAL=/dev/cu.usbserial-57130079311 python3 logger.py (change serial)
"""

import csv
import ctypes
import os
import socket
import struct
import sys
import threading
import time

# ── Config ────────────────────────────────────────────────────────────────────
LISTEN_PORT             = 9000
CSV_FILE                = "benchmark_packets.csv"
SERIAL_DEV              = sys.argv[1] if len(sys.argv) > 1 else os.getenv("ESP32_SERIAL")
SERIAL_LOG              = "serial_log.txt"
BAUD_RATE               = 115200
COMPLETE_GRACE_SEC      = 2.5

# ── Name maps ─────────────────────────────────────────────────────────────────
TRANSPORT = {
    0: "RAW_UDP",
    1: "CNMAT",
    2: "MICROOSC",
    3: "LO",
    4: "OSSIA",
    5: "TINYOSC",
    6: "OSCPKT",
}

MODE = {
    0: "10Hz",
    1: "100Hz",
    2: "500Hz",
    3: "1000Hz",
    4: "FAST",
    5: "PAY8",
    6: "PAY16",
    7: "MULTI4",
    8: "MULTI8",
    9: "BUNDLE2",
    10: "BUNDLE4",
    11: "RAMP_RATE",
    12: "RAMP_PAY",
    13: "RAMP_MULTI",
    14: "RAMP_BUNDLE",
}

# Mode groups
RATE_MODES    = [0, 1, 2, 3, 4]
PAYLOAD_MODES = [5, 6]
MULTI_MODES   = [7, 8]
BUNDLE_MODES  = [9, 10]

# Grouped modes.
TIMING_GROUP_MODES = {7, 8, 9, 10, 13, 14}

# ── Runtime events ───────────────────────────────────────────────────────────
# Threading model:
#   - UDP receive runs in the main thread.
#   - Serial logging runs in a background thread.
_benchmark_complete_event = threading.Event()
_run_meta_lock = threading.Lock()
_run_meta: dict = {}
_current_serial_run_id = None


class _Timespec(ctypes.Structure):
    _fields_ = [
        ("tv_sec", ctypes.c_long),
        ("tv_nsec", ctypes.c_long),
    ]

# ── Helper functions ─────────────────────────────────────────────────────────
def _run_key(run_id, _transport=None, _mode=None):
    return int(run_id)

def ensure_run_meta(run_id, transport=None, mode=None) -> dict:
    """Create or fetch the metadata dict for one run."""
    key = _run_key(run_id, transport, mode)
    with _run_meta_lock:
        return _run_meta.setdefault(key, {})


def update_run_meta(run_id, transport=None, mode=None, **fields):
    meta = ensure_run_meta(run_id, transport, mode)
    meta.update(fields)


def get_run_meta(run_id, transport=None, mode=None) -> dict:
    with _run_meta_lock:
        return dict(_run_meta.get(_run_key(run_id, transport, mode), {}))


def enable_kernel_rx_timestamps(sock: socket.socket) -> bool:
    """Enable Linux kernel receive timestamps when available."""
    # On Linux, prefer kernel socket timestamps over a userspace time.time_ns()
    # call after recvfrom(). Other platforms fall back automatically.
    opt = getattr(socket, "SO_TIMESTAMPNS", None)
    if not sys.platform.startswith("linux") or opt is None:
        return False
    try:
        sock.setsockopt(socket.SOL_SOCKET, opt, 1)
        return True
    except OSError:
        return False


def recv_packet(sock: socket.socket, use_kernel_timestamps: bool):
    """Receive one packet and return data, addr, and the best host RX timestamp."""
    if not use_kernel_timestamps:
        data, addr = sock.recvfrom(4096)
        return data, addr, time.time_ns()

    # recvmsg() lets us read the kernel-provided receive timestamp attached
    # to the packet on Linux via ancillary data.
    data, ancdata, _msg_flags, addr = sock.recvmsg(4096, 256)
    rx_ns = None
    scm_timestampns = getattr(socket, "SCM_TIMESTAMPNS", None)

    for level, ctype, cdata in ancdata:
        if level == socket.SOL_SOCKET and ctype == scm_timestampns:
            ts = _Timespec.from_buffer_copy(cdata[:ctypes.sizeof(_Timespec)])
            rx_ns = ts.tv_sec * 1_000_000_000 + ts.tv_nsec
            break

    if rx_ns is None:
        rx_ns = time.time_ns()
    return data, addr, rx_ns


# ── CSV output helpers ───────────────────────────────────────────────────────
def packet_csv_row(rx_ns, src_ip, payload_len, packet: dict):
    """Build one CSV row for one parsed packet.

    Includes packet fields, decoded labels, ramp metadata, and parse errors.
    """
    run_id = packet.get("run_id", "")
    # run_id is the only identity carried in the packet.
    # Transport and mode are recovered from earlier serial metadata.
    meta = get_run_meta(run_id) if isinstance(run_id, int) else {}
    transport = meta.get("transport", "")
    mode = meta.get("mode", "")
    seq = packet.get("seq", "")

    transport_name = TRANSPORT.get(transport, "") if isinstance(transport, int) else ""
    mode_name = MODE.get(mode, "") if isinstance(mode, int) else ""
    mode_label = tracker_mode_label(run_id) if isinstance(run_id, int) else ""
    logical_seq, addr_idx = decode_seq_fields(transport, mode, seq) if isinstance(transport, int) and isinstance(mode, int) and isinstance(seq, int) else ("", "")

    return [
        rx_ns,
        src_ip,
        payload_len,
        packet.get("proto", ""),
        int(packet["ok"]),
        packet.get("fail_reason", ""),
        packet.get("sample", ""),
        run_id,
        transport,
        transport_name,
        mode,
        mode_name,
        mode_label,
        mode_group(mode) if isinstance(mode, int) else "",
        int(mode in TIMING_GROUP_MODES) if isinstance(mode, int) else "",
        seq,
        logical_seq,
        addr_idx,
        packet.get("n_args", ""),
        packet.get("addr", ""),
        packet.get("send_start_us", ""),
        packet.get("prev_send_cost_us", ""),
        meta.get("rate_hz", ""),
        meta.get("interval_us", ""),
        meta.get("payload_args", ""),
        meta.get("multi_addrs", ""),
        meta.get("bundle_msgs", ""),
    ]

# ── Packet parsing and validation ────────────────────────────────────────────
MAX_RUN_ID = 1_000_000

OSC_BENCH_PREFIX = b"/bench"

def _valid_identity(run_id, seq) -> bool:
    """Check that run_id and seq are in a valid range."""
    return 0 <= run_id <= MAX_RUN_ID and 0 <= seq <= 0x7FFFFFFF

def _osc_pad4(n: int) -> int:
    return (n + 3) & ~3

def _parse_osc(data: bytes) -> dict:
    """Parse one OSC message carrying the benchmark's int32 identity payload."""
    n = len(data)
    # Check the packet label.
    if data[:6] != OSC_BENCH_PREFIX:
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    # Find where the OSC address string ends.
    try:
        addr_null = data.index(0, 1)
    except ValueError:
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    # Read the address and move to the next 4-byte boundary.
    addr_len = _osc_pad4(addr_null + 1)
    addr = data[:addr_null].decode("ascii", errors="replace")
    # Make sure there is still data left after the address.
    if addr_len >= n:
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    # Check the format description.
    tag_start = addr_len
    if data[tag_start:tag_start + 1] != b",":
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    # Find where the type tag string ends.
    try:
        tag_null = data.index(0, tag_start + 1)
    except ValueError:
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    # Find where the argument data starts.
    tag = data[tag_start + 1:tag_null].decode("ascii", errors="replace")
    tag_len = _osc_pad4(tag_null - tag_start + 1)
    data_off = tag_start + tag_len

    # This benchmark expects all arguments to be int32 values.
    if not all(c == "i" for c in tag):
        return dict(ok=False, fail_reason="bad_osc_header", sample=data[:16].hex())

    n_args = len(tag)
    # Make sure the packet is big enough for all declared arguments.
    if data_off + n_args * 4 > n:
        return dict(ok=False, fail_reason="truncated", sample=data[:16].hex())
    # We need at least the 4 benchmark fields.
    if n_args < 4:
        return dict(ok=False, fail_reason="too_few_args", sample=data[:16].hex())

    # Pull out the integer arguments.
    args = struct.unpack_from(f">{n_args}i", data, data_off)
    run_id, seq, send_start_us, prev_send_cost_us = args[0], args[1], args[2], args[3]

    # Reject packets with impossible benchmark identity fields.
    if not _valid_identity(run_id, seq):
        return dict(
            ok=False,
            fail_reason="bad_identity",
            run_id=run_id,
            seq=seq,
            sample=data[:16].hex(),
        )

    return dict(
        ok=True,
        proto="osc",
        addr=addr,
        n_args=n_args,
        run_id=run_id,
        seq=seq,
        send_start_us=send_start_us,
        prev_send_cost_us=prev_send_cost_us,
    )

def _parse_bundle(data: bytes) -> list:
    """Parse OSC bundle and return valid benchmark submessages."""
    if len(data) < 16 or data[:8] != b"#bundle\x00":
        return []
    pos = 16
    messages = []
    while pos + 4 <= len(data):
        msg_size = struct.unpack_from(">i", data, pos)[0]
        pos += 4
        if msg_size <= 0 or pos + msg_size > len(data):
            break
        p = _parse_osc(data[pos:pos + msg_size])
        if p["ok"]:
            messages.append(p)
        pos += msg_size
    return messages

def parse_packet(data: bytes) -> dict:
    """Parse one UDP packet as RAW, OSC, or invalid data."""

    length = len(data)
    is_osc = data[:6] == b"/bench"

    if length >= 16 and length % 4 == 0 and not is_osc and data[0:1] != b"/":
        n_ints = length // 4
        vals = struct.unpack(f"<{n_ints}I", data)
        run_id, seq, send_start_us, prev_send_cost_us = vals[0], vals[1], vals[2], vals[3]

        if not _valid_identity(run_id, seq):
            return dict(
                ok=False,
                fail_reason="bad_identity",
                run_id=run_id,
                seq=seq,
                sample=data[:16].hex(),
            )

        return dict(
            ok=True,
            proto="raw",
            n_args=n_ints,
            run_id=run_id,
            seq=seq,
            send_start_us=send_start_us,
            prev_send_cost_us=prev_send_cost_us,
        )

    if is_osc or data[0:1] == b"/":
        return _parse_osc(data)

    return dict(
        ok=False,
        proto="unknown",
        fail_reason="wrong_size",
        run_id=-1,
        seq=-1,
        sample=data[:16].hex(),
    )

def decode_seq_fields(transport: int, mode: int, seq: int):
    """Split RAW multi-address wireSeq into logical seq and addr index.

    RAW multi modes encode:
      wireSeq = (addr_idx << 16) | logical_seq
    All other modes keep seq unchanged.
    """
    if transport == 0 and mode in (7, 8, 13):
        return seq & 0xFFFF, seq >> 16
    return seq, None


def mode_group(mode: int) -> str:
    if mode in RATE_MODES:
        return "rate"
    if mode in PAYLOAD_MODES:
        return "payload"
    if mode in MULTI_MODES or mode == 13:
        return "multi"
    if mode in BUNDLE_MODES or mode == 14:
        return "bundle"
    return "unknown"


def tracker_mode_label(run_id: int) -> str:
    meta = get_run_meta(run_id)
    mode = meta.get("mode")
    if not isinstance(mode, int):
        return ""
    base = MODE.get(mode, str(mode))
    if mode == 11 and "rate_hz" in meta:
        return f"{base}[{meta['rate_hz']}Hz]"
    if mode == 12 and "payload_args" in meta:
        suffix = f" @ {meta['rate_hz']}Hz" if "rate_hz" in meta else ""
        return f"{base}[{meta['payload_args']} args{suffix}]"
    if mode == 13 and "multi_addrs" in meta:
        suffix = f" @ {meta['rate_hz']}Hz" if "rate_hz" in meta else ""
        return f"{base}[{meta['multi_addrs']} addrs{suffix}]"
    if mode == 14 and "bundle_msgs" in meta:
        suffix = f" @ {meta['rate_hz']}Hz" if "rate_hz" in meta else ""
        return f"{base}[{meta['bundle_msgs']} msgs{suffix}]"
    return base

# ── Serial-side ingestion ────────────────────────────────────────────────────
def serial_logger():
    """Background serial reader for ESP-side diagnostics and run metadata.

    The ESP32 emits structured rows for information that never appears on UDP:
    send failures, WiFi events, and run summaries.
    """
    try:
        import serial  # pyserial
    except ImportError:
        print("pyserial not installed — serial logging disabled")
        return

    ser = None
    fh = open(SERIAL_LOG, "w", buffering=1)

    try:
        while ser is None:
            try:
                ser = serial.Serial(SERIAL_DEV, BAUD_RATE, timeout=0.2)
            except Exception:
                time.sleep(0.5)

        while True:
            try:
                raw = ser.readline()
            except Exception:
                time.sleep(0.1)
                continue

            if not raw:
                continue

            try:
                line = raw.decode("utf-8", errors="replace").rstrip()
            except Exception:
                continue

            fh.write(line + "\n")

            if line == "BENCHMARK COMPLETE":
                _benchmark_complete_event.set()
                continue

            if line.startswith("=== MODE START"):
                # The ESP32 prints:
                #   === MODE START [run=ID] [TRANSPORT] MODE ...
                # We parse that line once and store the transport/mode for this run_id.
                try:
                    header = line.split("[run=")[1]
                    run_id = int(header.split("]")[0])
                    transport_name = line.split("] [", 1)[1].split("]", 1)[0]
                    mode_name = line.rsplit("] ", 1)[1].split("  warmup=", 1)[0]
                    transport = next((k for k, v in TRANSPORT.items() if v == transport_name), -1)
                    mode = next((k for k, v in MODE.items() if v == mode_name), -1)
                    if transport >= 0 and mode >= 0:
                        update_run_meta(
                            run_id, transport, mode,
                            transport=transport,
                            mode=mode,
                            transport_name=transport_name,
                            mode_name=mode_name,
                        )
                        global _current_serial_run_id
                        _current_serial_run_id = run_id
                except Exception:
                    pass
                continue

            if _current_serial_run_id and line.startswith("[RAMP_RATE"):
                try:
                    run_id = _current_serial_run_id
                    parts = line.replace("[", "").replace("]", "").split()
                    rate_hz = int(parts[3].replace("Hz", ""))
                    interval_us = int(parts[4].split("=")[1].replace("us", ""))
                    update_run_meta(run_id, rate_hz=rate_hz, interval_us=interval_us)
                except Exception:
                    pass
                continue

            if _current_serial_run_id and line.startswith("[RAMP_PAY"):
                try:
                    run_id = _current_serial_run_id
                    payload_args = int(line.split("] ", 1)[1].split(" args", 1)[0])
                    rate_hz = int(line.split("@", 1)[1].split("Hz", 1)[0])
                    interval_us = int(line.split("interval=", 1)[1].split("us", 1)[0])
                    update_run_meta(run_id, payload_args=payload_args, rate_hz=rate_hz, interval_us=interval_us)
                except Exception:
                    pass
                continue

            if _current_serial_run_id and line.startswith("[RAMP_MULTI"):
                try:
                    run_id = _current_serial_run_id
                    multi_addrs = int(line.split("] ", 1)[1].split(" addrs", 1)[0])
                    rate_hz = int(line.split("@", 1)[1].split("Hz", 1)[0])
                    interval_us = int(line.split("interval=", 1)[1].split("us", 1)[0])
                    update_run_meta(run_id, multi_addrs=multi_addrs, rate_hz=rate_hz, interval_us=interval_us)
                except Exception:
                    pass
                continue

            if _current_serial_run_id and line.startswith("[RAMP_BDL"):
                try:
                    run_id = _current_serial_run_id
                    bundle_msgs = int(line.split("] ", 1)[1].split(" msgs", 1)[0])
                    rate_hz = int(line.split("@", 1)[1].split("Hz", 1)[0])
                    interval_us = int(line.split("interval=", 1)[1].split("us", 1)[0])
                    update_run_meta(run_id, bundle_msgs=bundle_msgs, rate_hz=rate_hz, interval_us=interval_us)
                except Exception:
                    pass
                continue

            if line.startswith("S,"):
                parts = line.split(",")
                if len(parts) == 8:
                    _, rid, tid, mid, elapsed_ms, attempted, ok, error_count = parts
                    update_run_meta(
                        rid, tid, mid,
                        elapsed_ms=int(elapsed_ms),
                        attempted=int(attempted),
                        ok=int(ok),
                        error_count=int(error_count),
                    )
                continue

            if line.startswith("T,"):
                parts = line.split(",")
                if len(parts) == 5:
                    _, rid, tid, mid, elapsed_ms = parts
                    update_run_meta(rid, tid, mid, elapsed_ms=int(elapsed_ms))
                continue

            if line.startswith("N,"):
                continue

            if line.startswith("W,"):
                continue

    finally:
        try:
            fh.close()
        except Exception:
            pass
        try:
            if ser is not None:
                ser.close()
        except Exception:
            pass

# ── Main receiver loop ───────────────────────────────────────────────────────
def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    use_kernel_timestamps = enable_kernel_rx_timestamps(sock)
    sock.bind(("", LISTEN_PORT))
    sock.settimeout(0.5)
    print(f"Listening on UDP port {LISTEN_PORT}  →  {CSV_FILE}")
    if use_kernel_timestamps:
        print("Host RX timestamps: Linux kernel socket timestamps enabled")

    complete_deadline = None
    seen_runs = set()

    if SERIAL_DEV:
        print(f"Serial: {SERIAL_DEV}  (waiting for device...)")
        threading.Thread(target=serial_logger, daemon=True).start()
    else:
        print("Serial: not configured  (no error data)")

    csv_fh = open(CSV_FILE, "w", newline="")
    writer = csv.writer(csv_fh)
    # receiver_rx_ns is the host-side receive timestamp for this packet.
    # On Linux we use the kernel socket timestamp when available; otherwise
    # we fall back to time.time_ns() right after the receive call returns.
    writer.writerow([
        "receiver_rx_ns", "src_ip", "payload_len", "proto",
        "parse_ok", "fail_reason", "sample",
        "run_id", "transport", "transport_name",
        "mode", "mode_name", "mode_label", "mode_group", "timing_grouped",
        "seq", "logical_seq", "addr_idx", "n_args", "addr",
        "send_start_us", "prev_send_cost_us",
        "rate_hz", "interval_us", "payload_args", "multi_addrs", "bundle_msgs",
    ])

    try:
        while True:
            try:
                data, addr, rx_ns = recv_packet(sock, use_kernel_timestamps)
            except socket.timeout:
                if _benchmark_complete_event.is_set() and complete_deadline is None:
                    complete_deadline = time.monotonic() + COMPLETE_GRACE_SEC
                if complete_deadline is not None and time.monotonic() >= complete_deadline:
                    break
                continue

            if data[:8] == b"#bundle\x00":
                packets = _parse_bundle(data)
            else:
                packets = [parse_packet(data)]

            for bp in packets:
                writer.writerow(packet_csv_row(rx_ns, addr[0], len(data), bp))
            csv_fh.flush()

            p = packets[0] if packets else parse_packet(data)
            meta = get_run_meta(p.get("run_id", -1)) if p.get("ok") else {}
            tname = TRANSPORT.get(meta.get("transport", -1), str(meta.get("transport", "")))
            mname = MODE.get(meta.get("mode", -1), str(meta.get("mode", "")))

            if not p["ok"]:
                if len(data) == 1:
                    continue
                reason = p.get("fail_reason", "unknown")
                sample = p.get("sample", data[:16].hex())
                print(f"[{reason}] {len(data)}B from {addr[0]}: {sample}")
                continue

            if p["run_id"] not in seen_runs:
                seen_runs.add(p["run_id"])
                print(f"[RX] run={p['run_id']} [{tname}] {mname}")

    except KeyboardInterrupt:
        print("\nInterrupted.")

    finally:
        try:
            csv_fh.close()
        except Exception:
            pass
        try:
            sock.close()
        except Exception:
            pass

if __name__ == "__main__":
    main()
