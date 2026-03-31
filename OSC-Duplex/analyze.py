"""
analyze.py — Offline analysis for the ESP32 OSC benchmark.

SUMMARY
-------
This script reads the two artifacts produced by the benchmark logger:
  - `serial_log.txt` for ESP32-side send failures, summaries, and
    WiFi context.
  - `benchmark_packets.csv` for host-side receive timing and packet metadata.

Details:
  - send cost and send interval come from ESP32 timestamps
  - receive interval and receive loss come from the host packet log
  - WiFi snapshots show when link state may have affected a run
  - fixed-mode tables use the median across repeated runs for each
    transport/mode pair
  - the final verdict compares OSC libraries across fixed-mode metric groups,
    adds one winner per ramp family based on the highest sustainable step,
    allows ties, and then totals those wins into an overall score

METRICS
---------------
The most meaningful comparison metrics for this benchmark are:
  1. send cost         How much work the library adds to each send.
  2. send interval     How well the sender maintains its target schedule.
  3. total loss        What fraction of expected units never arrived.
  4. breakpoints       Where ramp modes stop being sustainable.

Receive interval is still reported, but only as secondary context. It is more
easily distorted by host scheduling, socket buffering, and WiFi delivery
clustering than the sender-side timestamps embedded by the ESP32.

ERRORS
------
This analyzer keeps three error categories separate:
  - send fail   ESP-side `E` row with code `1` (`SEND_FAIL`)
  - build fail  ESP-side `E` row with code `2` (`BUILD_FAIL`)
  - parse fail  CSV row where `parse_ok != 1`; the logger stores the reason in
                `fail_reason` (for example `bad_osc_header`, `truncated`,
                `too_few_args`, `bad_identity`, or `wrong_size`)

Parse failures stay out of timing metrics, but they still affect loss
indirectly: only valid parsed units count as delivered units for a run.

WHAT THIS VERSION MATCHES
-------------------------
The current benchmark has 15 modes:
  fixed modes: 10Hz, 100Hz, 500Hz, 1000Hz, FAST, PAY8, PAY16,
               MULTI4, MULTI8, BUNDLE2, BUNDLE4
  ramp modes:  RAMP_RATE, RAMP_PAY, RAMP_MULTI, RAMP_BUNDLE

Usage:
  python3 analyze.py
  python3 analyze.py --serial serial_log.txt --csv benchmark_packets.csv
"""

import argparse
import csv
import sys
from collections import defaultdict

# ── Config and benchmark constants ───────────────────────────────────────────
SEND_SPIKE_US = 1000
LOSS_BREAKPOINT_PCT = 1.0
RAMP_COST_RATIO_LIMIT = 0.80
RAMP_ERROR_RATE_LIMIT = 5.0
RAMP_ABS_COST_LIMIT_MS = 5.0

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

RATE_MODES = [0, 1, 2, 3, 4]
PAYLOAD_MODES = [5, 6]
MULTI_MODES = [7, 8]
BUNDLE_MODES = [9, 10]
RAMP_MODES = [11, 12, 13, 14]
OSC_TIDS = [1, 2, 3, 4, 5, 6]
ALL_TIDS = [0, 1, 2, 3, 4, 5, 6]
FIXED_MULTI_ADDRS = {7: 4, 8: 8}
FIXED_BUNDLE_MSGS = {9: 2, 10: 4}
TIMING_GROUP_MODES = {7, 8, 9, 10, 13, 14}

# ── Generic stats helpers ────────────────────────────────────────────────────
def median(vals):
    if not vals:
        return 0
    s = sorted(vals)
    n = len(s)
    return (s[n // 2] + s[(n - 1) // 2]) / 2


def stats(vals):
    """Return min/median/avg/max/jitter/count for one numeric sample list."""
    if not vals:
        return dict(min=0, median=0, avg=0, max=0, jitter=0, count=0)
    mn = min(vals)
    mx = max(vals)
    return dict(
        min=mn,
        median=median(vals),
        avg=sum(vals) / len(vals),
        max=mx,
        jitter=mx - mn,
        count=len(vals),
    )


def pct(n, total):
    return 100.0 * n / total if total else 0.0


def safe_int(value, default=None):
    try:
        if value in ("", None):
            return default
        return int(value)
    except (TypeError, ValueError):
        return default


def fmt_num(value, scale=1.0, places=3):
    if value is None:
        return "—"
    return f"{value / scale:.{places}f}"


# ── Per-run analysis model ───────────────────────────────────────────────────
class ModeData:
    """All observed data for one benchmark run.
    """

    def __init__(self, run_id, transport, mode):
        self.run_id = run_id
        self.transport = transport
        self.mode = mode

        # Error sources:
        #   errors              ESP-side `E` rows for send/build failures
        #   parse_fail_reasons  CSV rows that failed packet parsing on the host
        self.errors = []      # (seq, time_us, code)
        self.summary_attempted = None
        self.summary_send_ok = None
        self.summary_error_count = None
        self.elapsed_ms = None
        self.wifi_start = None
        self.wifi_end = None
        self.wifi_event_count = 0

        # Packet-side metadata copied from the CSV. 
        self.mode_name = MODE.get(mode, str(mode))
        self.mode_label = self.mode_name
        self.mode_group = ""
        self.rate_hz = None
        self.interval_us = None
        self.payload_args = None
        self.multi_addrs = None
        self.bundle_msgs = None
        self.timing_grouped = mode in TIMING_GROUP_MODES

        # Send-side timing reconstructed from embedded packet fields.
        self.send_costs = []
        self.send_starts = []
        self.spikes = 0
        self._timing_keys = set()

        # Receive-side observations.
        self.rx_rows = 0
        self.rx_ns = []
        self.rx_tick_ns = []
        self.rx_seqs = set()
        self.addr_rx = defaultdict(int)
        self.parse_fail_reasons = defaultdict(int)  # fail_reason -> count

    def update_from_csv_row(self, row):
        """Capture per-run metadata from any valid packet row.
        """
        self.transport = safe_int(row.get("transport"), self.transport)
        self.mode = safe_int(row.get("mode"), self.mode)
        self.mode_name = row.get("mode_name", self.mode_name) or self.mode_name
        self.mode_label = row.get("mode_label", self.mode_label) or self.mode_label
        self.mode_group = row.get("mode_group", self.mode_group) or self.mode_group
        self.timing_grouped = row.get("timing_grouped", "0") == "1" or self.timing_grouped
        self.rate_hz = safe_int(row.get("rate_hz"), self.rate_hz)
        self.interval_us = safe_int(row.get("interval_us"), self.interval_us)
        self.payload_args = safe_int(row.get("payload_args"), self.payload_args)
        self.multi_addrs = safe_int(row.get("multi_addrs"), self.multi_addrs)
        self.bundle_msgs = safe_int(row.get("bundle_msgs"), self.bundle_msgs)

    def add_packet(self, row):
        """Fold one valid CSV packet row into the run statistics.
        """
        self.update_from_csv_row(row)

        seq = safe_int(row.get("seq"))
        logical_seq = safe_int(row.get("logical_seq"), seq)
        addr_idx = safe_int(row.get("addr_idx"))
        rx_ns = safe_int(row.get("receiver_rx_ns"), safe_int(row.get("mac_rx_ns")))
        send_start_us = safe_int(row.get("send_start_us"))
        prev_send_cost_us = safe_int(row.get("prev_send_cost_us"))
        addr = row.get("addr", "")

        if seq is not None:
            self.rx_seqs.add(seq)

        self.rx_rows += 1
        if rx_ns is not None:
            self.rx_ns.append(rx_ns)

        # timing_key is the "count this send once" key.
        # Grouped modes use logical_seq because several rows can belong to one send.
        # Other modes use seq because one row matches one send.
        if self.timing_grouped:
            timing_key = logical_seq
        else:
            timing_key = seq

        if rx_ns is not None and timing_key is not None and timing_key not in self._timing_keys:
            self.rx_tick_ns.append(rx_ns)

        if timing_key is not None and timing_key not in self._timing_keys:
            self._timing_keys.add(timing_key)
            if send_start_us is not None:
                self.send_starts.append(send_start_us)
            if logical_seq and logical_seq > 0 and prev_send_cost_us is not None:
                self.send_costs.append(prev_send_cost_us)
                if prev_send_cost_us > SEND_SPIKE_US:
                    self.spikes += 1

        # Multi-address delivery is counted per address. 
        if self.expected_units_per_attempt() > 1 and self.mode in MULTI_MODES + [13]:
            if addr_idx is not None:
                key = f"/bench/{addr_idx}"
            else:
                key = addr or "/bench"
            self.addr_rx[key] += 1

    def add_error(self, seq, time_us, code):
        self.errors.append((seq, time_us, code))

    def add_parse_failure(self, row):
        """Track a CSV row that failed to parse as a valid benchmark packet."""
        self.update_from_csv_row(row)
        reason = row.get("fail_reason", "unknown") or "unknown"
        self.parse_fail_reasons[reason] += 1

    def add_summary(self, attempted, send_ok, error_count):
        self.summary_attempted = attempted
        self.summary_send_ok = send_ok
        self.summary_error_count = error_count

    def add_elapsed_ms(self, elapsed_ms):
        self.elapsed_ms = elapsed_ms

    def add_wifi_snapshot(self, phase, snapshot):
        if phase == "start":
            self.wifi_start = snapshot
        elif phase == "end":
            self.wifi_end = snapshot

    # ── Derived helpers ───────────────────────────────────────────────────────
    def expected_units_per_attempt(self):
        """Return how many receive-side units one logical send should create."""
        if self.multi_addrs:
            return self.multi_addrs
        if self.bundle_msgs:
            return self.bundle_msgs
        if self.mode in FIXED_MULTI_ADDRS:
            return FIXED_MULTI_ADDRS[self.mode]
        if self.mode in FIXED_BUNDLE_MSGS:
            return FIXED_BUNDLE_MSGS[self.mode]
        return 1

    def attempted_packets(self):
        """Return how many logical send attempts the run made.
        """
        if self.summary_attempted is not None:
            return self.summary_attempted
        return len(self._timing_keys)

    def received_units(self):
        """Count valid delivered units used for loss accounting.

        Parse-failure rows are not counted here, so attributable malformed
        packets reduce the delivered-unit total and therefore increase loss.
        """
        if self.expected_units_per_attempt() > 1 and self.addr_rx:
            return sum(self.addr_rx.values())
        return self.rx_rows

    def expected_units(self):
        return self.attempted_packets() * self.expected_units_per_attempt()

    def lost_units(self):
        return max(0, self.expected_units() - self.received_units())

    def total_loss_pct(self):
        return pct(self.lost_units(), self.expected_units())

    def error_counts(self):
        send_fail = sum(1 for _, _, code in self.errors if int(code) == 1)
        build_fail = sum(1 for _, _, code in self.errors if int(code) == 2)
        parse_fail = sum(self.parse_fail_reasons.values())
        return send_fail, build_fail, parse_fail

    def send_cost_stats(self):
        return stats(self.send_costs)

    def send_interval_stats(self):
        # Sender-side intervals are reconstructed from ESP timestamps 
        if len(self.send_starts) < 2:
            return stats([])
        s = sorted(self.send_starts)
        return stats([s[i] - s[i - 1] for i in range(1, len(s))])

    def rx_interval_stats(self):
        # Receive intervals are still useful, but only as host/network context.
        # Grouped modes are deduplicated to one timestamp per logical send tick.
        source = self.rx_tick_ns if self.timing_grouped else self.rx_ns
        if len(source) < 2:
            return stats([])
        s = sorted(source)
        return stats([(s[i] - s[i - 1]) / 1000 for i in range(1, len(s))])

    def run_sort_value(self):
        """Sort ramp steps."""
        if self.mode == 11 and self.rate_hz is not None:
            return self.rate_hz
        if self.mode == 12 and self.payload_args is not None:
            return (self.rate_hz or 0) * 1000 + self.payload_args
        if self.mode == 13 and self.multi_addrs is not None:
            return (self.rate_hz or 0) * 1000 + self.multi_addrs
        if self.mode == 14 and self.bundle_msgs is not None:
            return (self.rate_hz or 0) * 1000 + self.bundle_msgs
        for value in (self.rate_hz, self.payload_args, self.multi_addrs, self.bundle_msgs):
            if value is not None:
                return value
        return self.run_id


# ── Input parsing ────────────────────────────────────────────────────────────
def parse_serial(path):
    """Parse `serial_log.txt` into per-run data objects.

    Rows currently used by the analyzer:
      E,...  one ESP send/build error for a run
      S,...  one per-run summary from the ESP
             elapsed_ms = measured run duration in milliseconds
             attempted = sends attempted, send_ok = sends that succeeded
      N,...  WiFi snapshot at run start or end
      W,...  WiFi event log row
    """
    data = {}
    wifi_events_by_run = defaultdict(int)

    def get(rid, tid, mid):
        key = rid
        if key not in data:
            data[key] = ModeData(rid, tid, mid)
        else:
            data[key].transport = tid
            data[key].mode = mid
        return data[key]

    try:
        with open(path) as fh:
            for raw in fh:
                line = raw.strip()
                if "] " in line and line.startswith("["):
                    line = line.split("] ", 1)[1]

                parts = line.split(",")
                if not parts:
                    continue

                tag = parts[0]
                try:
                    if tag == "E" and len(parts) == 7:
                        _, rid, tid, mid, seq, time_us, code = parts
                        get(int(rid), int(tid), int(mid)).add_error(
                            int(seq), int(time_us), int(code)
                        )
                    elif tag == "S" and len(parts) == 8:
                        _, rid, tid, mid, elapsed_ms, attempted, send_ok, error_count = parts
                        get(int(rid), int(tid), int(mid)).add_summary(
                            int(attempted), int(send_ok), int(error_count)
                        )
                        get(int(rid), int(tid), int(mid)).add_elapsed_ms(int(elapsed_ms))
                    elif tag == "N" and len(parts) == 11:
                        _, rid, tid, mid, phase, time_ms, wifi_status, rssi, channel, disconnects, got_ip = parts
                        get(int(rid), int(tid), int(mid)).add_wifi_snapshot(
                            phase,
                            dict(
                                time_ms=int(time_ms),
                                wifi_status=wifi_status,
                                rssi=int(rssi),
                                channel=int(channel),
                                disconnects=int(disconnects),
                                got_ip=int(got_ip),
                            ),
                        )
                    elif tag == "W" and len(parts) == 7:
                        _, rid, _time_ms, _event_id, _wifi_status, _rssi, _channel = parts
                        wifi_events_by_run[int(rid)] += 1
                except ValueError:
                    # Serial logs are append-only and may contain partial lines
                    # when the program exits. Ignore malformed rows and keep the
                    # good data rather than failing the whole analysis.
                    continue
    except FileNotFoundError:
        print(f"[warn] {path} not found — no serial data")

    for run in data.values():
        run.wifi_event_count = wifi_events_by_run.get(run.run_id, 0)

    return data


def parse_csv(path, runs):
    """Parse `benchmark_packets.csv` and merge packet observations into runs.

    Valid rows feed the timing metrics and the successful receive counts.
    Parse-failure rows are tracked separately as receive-side errors. 
    """
    try:
        with open(path, newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                run_id = safe_int(row.get("run_id"))
                if run_id is None:
                    continue

                key = run_id
                if key not in runs:
                    transport = safe_int(row.get("transport"), -1)
                    mode = safe_int(row.get("mode"), -1)
                    runs[key] = ModeData(run_id, transport, mode)

                if row.get("parse_ok", "0") == "1":
                    runs[key].add_packet(row)
                else:
                    runs[key].add_parse_failure(row)
    except FileNotFoundError:
        print(f"[warn] {path} not found — no packet data")

    return runs


# ── Grouping and run selection helpers ───────────────────────────────────────
def group_by_tm(all_data):
    """Regroup concrete runs by (transport, mode) for comparison tables."""
    grouped = defaultdict(list)
    for _rid, run in all_data.items():
        tid = run.transport
        mid = run.mode
        if tid is None or mid is None or tid < 0 or mid < 0:
            continue
        grouped[(tid, mid)].append(run)
    return grouped


def median_of_values(values):
    """Return the median of the non-None values in one list."""
    vals = [value for value in values if value is not None]
    if not vals:
        return None
    return median(vals)


def aggregate_metric_across_runs(runs, metric_fn):
    """Take the median of one metric across repeated fixed runs."""
    metrics = [metric_fn(run) for run in runs]
    metrics = [metric for metric in metrics if metric and metric.get("count", 0)]
    if not metrics:
        return None
    return dict(
        min=median_of_values(metric["min"] for metric in metrics),
        median=median_of_values(metric["median"] for metric in metrics),
        avg=median_of_values(metric["avg"] for metric in metrics),
        max=median_of_values(metric["max"] for metric in metrics),
        jitter=median_of_values(metric["jitter"] for metric in metrics),
        count=len(metrics),
    )


def aggregate_value_across_runs(runs, value_fn):
    """Take the median of one scalar across repeated fixed runs."""
    return median_of_values(value_fn(run) for run in runs)


def aggregate_error_counts_across_runs(runs):
    """Take the median error counts across repeated fixed runs."""
    triplets = [run.error_counts() for run in runs]
    if not triplets:
        return None
    return (
        int(round(median([t[0] for t in triplets]))),
        int(round(median([t[1] for t in triplets]))),
        int(round(median([t[2] for t in triplets]))),
    )


# ── Output formatting helpers ────────────────────────────────────────────────
WIDTH = 86
COL = 10


def header(title):
    print(f"\n{'═' * WIDTH}")
    print(f"  {title}")
    print(f"{'═' * WIDTH}")


def subheader(title):
    print(f"\n  {title}")
    print(f"  {'─' * (WIDTH - 2)}")


def col_header(tids):
    print(f"  {'':10}", end="")
    for tid in tids:
        print(f"  {TRANSPORT[tid]:>{COL}}", end="")
    print()


def row_cell(value, places=3):
    if value is None:
        return f"  {'—':>{COL}}"
    return f"  {value:>{COL}.{places}f}"


def row_cell_text(text):
    return f"  {text:>{COL}}"


def print_table_med_avg(title, modes, tids, runs_by_tm, metric_fn, unit="ms"):
    subheader(f"{title}  [{unit}]  (median / mean)")
    col_header(tids)
    for mid in modes:
        print(f"  {MODE[mid]:<10}med", end="")
        for tid in tids:
            runs = runs_by_tm.get((tid, mid), [])
            metric = aggregate_metric_across_runs(runs, metric_fn) if runs else None
            print(row_cell(metric["median"] if metric else None), end="")
        print()

        print(f"  {'':10}avg", end="")
        for tid in tids:
            runs = runs_by_tm.get((tid, mid), [])
            metric = aggregate_metric_across_runs(runs, metric_fn) if runs else None
            print(row_cell(metric["avg"] if metric else None), end="")
        print()


def print_table(title, modes, tids, runs_by_tm, value_fn, unit="ms", note=""):
    extra = f"  {note}" if note else ""
    subheader(f"{title}  [{unit}]{extra}")
    col_header(tids)
    for mid in modes:
        print(f"  {MODE[mid]:<10}", end="")
        for tid in tids:
            runs = runs_by_tm.get((tid, mid), [])
            value = aggregate_value_across_runs(runs, value_fn) if runs else None
            print(row_cell(value), end="")
        print()


def print_error_breakdown(modes, tids, runs_by_tm):
    subheader("ERROR BREAKDOWN  [median SEND_FAIL / BUILD_FAIL / PARSE_FAIL]")
    col_header(tids)
    for mid in modes:
        print(f"  {MODE[mid]:<10}", end="")
        for tid in tids:
            runs = runs_by_tm.get((tid, mid), [])
            if runs:
                send_fail, build_fail, parse_fail = aggregate_error_counts_across_runs(runs)
                cell = (
                    f"{send_fail}/{build_fail}/{parse_fail}"
                    if (send_fail or build_fail or parse_fail)
                    else "ok"
                )
            else:
                cell = "—"
            print(row_cell_text(cell), end="")
        print()


def print_multi_breakdown(modes, tids, runs_by_tm):
    subheader("MULTI DELIVERY BREAKDOWN  [median received units per address]")
    for mid in modes:
        print(f"\n  {MODE[mid]}:")
        for tid in tids:
            runs = runs_by_tm.get((tid, mid), [])
            if not runs:
                continue
            n_addrs = max(run.expected_units_per_attempt() for run in runs)
            if not n_addrs:
                continue
            parts = []
            for i in range(n_addrs):
                key = f"/bench/{i}"
                value = median_of_values(run.addr_rx.get(key, 0) for run in runs)
                parts.append(f"{i}:{int(round(value)) if value is not None else 0}")
            print(f"    {TRANSPORT[tid]:<10}  {'  '.join(parts)}")


def print_fixed_mode_sections(runs_by_tm):
    header("RATE MODES")
    print("  These tables use the median across repeated fixed runs")
    print("  for each transport/mode pair.")
    print("  Primary comparison metrics come from the ESP32 timestamps embedded in packets.")

    print_table_med_avg(
        "SEND COST",
        RATE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.send_cost_stats()),
    )
    print_table(
        "SEND COST jitter",
        RATE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: to_ms(run.send_cost_stats()["jitter"]),
    )
    print_table_med_avg(
        "SEND INTERVAL",
        RATE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.send_interval_stats()),
    )
    print_table(
        "TOTAL LOSS",
        RATE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: run.total_loss_pct(),
        unit="%",
        note="expected units minus received units, using ESP32 attempt counts when available",
    )
    print_error_breakdown(RATE_MODES, ALL_TIDS, runs_by_tm)

    subheader("SECONDARY NETWORK CONTEXT")
    print("  Receive interval is useful for spotting host/network-side clustering,")
    print("  but it is a noisier metric than send interval and should not be used")
    print("  as the main library ranking signal.")
    print_table_med_avg(
        "RECEIVE INTERVAL",
        RATE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.rx_interval_stats()),
    )

    header("PAYLOAD SCALING")
    print_table_med_avg(
        "SEND COST",
        PAYLOAD_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.send_cost_stats()),
    )
    print_table(
        "TOTAL LOSS",
        PAYLOAD_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: run.total_loss_pct(),
        unit="%",
    )

    header("MULTI-ADDRESS MODES")
    print_table_med_avg(
        "CYCLE COST",
        MULTI_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.send_cost_stats()),
    )
    print_table(
        "PER-ADDRESS COST avg",
        MULTI_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: (
            to_ms(run.send_cost_stats()["avg"]) / run.expected_units_per_attempt()
            if run.expected_units_per_attempt()
            else None
        ),
    )
    print_table(
        "TOTAL LOSS",
        MULTI_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: run.total_loss_pct(),
        unit="%",
        note="loss is measured against attempted ticks × addresses per tick",
    )
    print_multi_breakdown(MULTI_MODES, ALL_TIDS, runs_by_tm)

    header("BUNDLE MODES")
    print("  Bundle send timing is deduplicated per logical bundle send.")
    print("  Loss is measured in delivered bundle members, not just UDP packets.")
    print("  Blank cells usually mean that transport did not participate in bundle mode.")
    print_table_med_avg(
        "BUNDLE COST",
        BUNDLE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: convert_stats_to_ms(run.send_cost_stats()),
    )
    print_table(
        "TOTAL LOSS",
        BUNDLE_MODES,
        ALL_TIDS,
        runs_by_tm,
        lambda run: run.total_loss_pct(),
        unit="%",
        note="loss is measured against attempted bundles × messages per bundle",
    )


def print_ramp_sections(runs_by_tm):
    header("RAMP RUNS")
    print("  Each row below is one measured ramp step.")
    print("  The strongest readout here is breakpoint behavior: where loss starts")
    print("  to rise, where send cost exceeds the ramp limit, or where a final")
    print("  step never completed enough attempts to be a valid measurement.")

    for mid in RAMP_MODES:
        subheader(MODE[mid])
        rows = []
        for tid in ALL_TIDS:
            for run in runs_by_tm.get((tid, mid), []):
                rows.append(run)

        if not rows:
            print("  No runs found.")
            continue

        rows.sort(key=lambda run: (run.transport, run.run_sort_value(), run.run_id))
        print(
            "  "
            f"{'Library':<10}  {'Run':>4}  {'Step':<24}  {'avg send':>8}  {'loss%':>7}  "
            f"{'ok/att':>8}  {'WiFi':<16}"
        )
        print(f"  {'─' * 82}")

        for run in rows:
            attempted = run.attempted_packets()
            send_ok = run.summary_send_ok if run.summary_send_ok is not None else "—"
            wifi = wifi_brief(run)
            print(
                "  "
                f"{TRANSPORT[run.transport]:<10}  "
                f"{run.run_id:>4}  "
                f"{run.mode_label:<24}  "
                f"{fmt_num(run.send_cost_stats()['avg'], scale=1000):>8}  "
                f"{run.total_loss_pct():>7.3f}  "
                f"{str(send_ok) + '/' + str(attempted):>8}  "
                f"{wifi:<16}"
            )

        print_breakpoints(mid, rows)


def print_breakpoints(mode, rows):
    """Report the first step where each transport becomes unsustainable.

    We distinguish between:
      - loss breakpoint
      - cost/scheduling breakpoint
      - incomplete final step
      - no breakpoint within tested steps
    """
    print(
        f"\n  Breakpoint (loss > {LOSS_BREAKPOINT_PCT:.1f}% or ramp stop threshold reached):"
    )
    for tid in ALL_TIDS:
        candidates = [run for run in rows if run.transport == tid]
        if not candidates:
            continue
        candidates.sort(key=lambda run: (run.run_sort_value(), run.run_id))
        hit = next((run for run in candidates if ramp_stop_reason(mode, run)), None)
        last = candidates[-1]

        if hit is None:
            print(
                f"    {TRANSPORT[tid]:<10}  no breakpoint within measured steps "
                f"(max tested: {last.mode_label})"
            )
            continue

        reason = ramp_stop_reason(mode, hit)
        if reason == "incomplete":
            prev = None
            hit_idx = candidates.index(hit)
            if hit_idx > 0:
                prev = candidates[hit_idx - 1]
            if prev is not None:
                print(
                    f"    {TRANSPORT[tid]:<10}  incomplete at {hit.mode_label}; "
                    f"last complete step: {prev.mode_label}"
                )
            else:
                print(
                    f"    {TRANSPORT[tid]:<10}  first measured step was incomplete "
                    f"({hit.mode_label})"
                )
            continue

        avg_send_ms = to_ms(hit.send_cost_stats()["avg"])
        cost_limit_ms = ramp_cost_limit_ms(mode, hit)
        if reason == "loss":
            detail = f"{hit.total_loss_pct():.3f}% loss"
        elif reason == "cost":
            detail = f"{avg_send_ms:.3f}ms avg send > {cost_limit_ms:.3f}ms limit"
        else:
            detail = (
                f"{avg_send_ms:.3f}ms avg send > {cost_limit_ms:.3f}ms limit "
                f"and {hit.total_loss_pct():.3f}% loss"
            )

        print(
            f"    {TRANSPORT[tid]:<10}  first breakpoint at {hit.mode_label} "
            f"({reason}: {detail})"
        )


def aggregate_metric_field_for_modes(runs_by_tm, tid, modes, metric_fn, field):
    """Take the median of one metric field across a family of fixed modes."""
    values = []
    for mid in modes:
        runs = runs_by_tm.get((tid, mid), [])
        metric = aggregate_metric_across_runs(runs, metric_fn) if runs else None
        if metric is not None:
            values.append(metric[field])
    return median_of_values(values)


def aggregate_value_for_modes(runs_by_tm, tid, modes, value_fn):
    """Take the median of one scalar across a family of fixed modes."""
    values = []
    for mid in modes:
        runs = runs_by_tm.get((tid, mid), [])
        value = aggregate_value_across_runs(runs, value_fn) if runs else None
        if value is not None:
            values.append(value)
    return median_of_values(values)


def metric_winners(rows, key):
    """Return the winning rows for one lower-is-better metric, allowing ties."""
    usable = [row for row in rows if row.get(key) is not None]
    if not usable:
        return []
    best = min(row[key] for row in usable)
    tol = max(1e-9, abs(best) * 1e-6)
    return [row for row in usable if abs(row[key] - best) <= tol]


def metric_winners_high(rows, key):
    """Return the winning rows for one higher-is-better metric, allowing ties."""
    usable = [row for row in rows if row.get(key) is not None]
    if not usable:
        return []
    best = max(row[key] for row in usable)
    tol = max(1e-9, abs(best) * 1e-6)
    return [row for row in usable if abs(row[key] - best) <= tol]


def ramp_survival_score(mode, runs):
    """Score one library within one ramp mode by its last sustainable step.

    Higher is better, but scores are only compared within the same ramp family.
    """
    if not runs:
        return None
    candidates = sorted(runs, key=lambda run: (run.run_sort_value(), run.run_id))
    first_bad = next((run for run in candidates if ramp_stop_reason(mode, run)), None)
    if first_bad is None:
        return candidates[-1].run_sort_value()
    bad_idx = candidates.index(first_bad)
    if ramp_stop_reason(mode, first_bad) == "incomplete":
        bad_idx -= 1
    if bad_idx < 0:
        return None
    return candidates[bad_idx].run_sort_value()


def print_verdict(runs_by_tm):
    header("VERDICT  (OSC libraries, fixed + ramp)")
    print("  Each metric below uses the median across repeated fixed runs.")
    print("  Ties are allowed; tied libraries share the win for that metric.")
    print("  Ramp winners are chosen by the highest sustainable step reached")
    print("  within each ramp family.")

    rows = []
    for tid in OSC_TIDS:
        rows.append(
            dict(
                tid=tid,
                name=TRANSPORT[tid],
                rate_send_avg=aggregate_metric_field_for_modes(
                    runs_by_tm, tid, RATE_MODES, lambda run: run.send_cost_stats(), "avg"
                ),
                rate_send_jitter=aggregate_metric_field_for_modes(
                    runs_by_tm, tid, RATE_MODES, lambda run: run.send_cost_stats(), "jitter"
                ),
                rate_itvl_jitter=aggregate_metric_field_for_modes(
                    runs_by_tm, tid, RATE_MODES, lambda run: run.send_interval_stats(), "jitter"
                ),
                rate_loss=aggregate_value_for_modes(
                    runs_by_tm, tid, RATE_MODES, lambda run: run.total_loss_pct()
                ),
                payload_send_avg=aggregate_metric_field_for_modes(
                    runs_by_tm, tid, PAYLOAD_MODES, lambda run: run.send_cost_stats(), "avg"
                ),
                payload_loss=aggregate_value_for_modes(
                    runs_by_tm, tid, PAYLOAD_MODES, lambda run: run.total_loss_pct()
                ),
                multi_per_addr_avg=aggregate_value_for_modes(
                    runs_by_tm,
                    tid,
                    MULTI_MODES,
                    lambda run: (
                        to_ms(run.send_cost_stats()["avg"]) / run.expected_units_per_attempt()
                        if run.expected_units_per_attempt()
                        else None
                    ),
                ),
                multi_loss=aggregate_value_for_modes(
                    runs_by_tm, tid, MULTI_MODES, lambda run: run.total_loss_pct()
                ),
                bundle_send_avg=aggregate_metric_field_for_modes(
                    runs_by_tm, tid, BUNDLE_MODES, lambda run: run.send_cost_stats(), "avg"
                ),
                bundle_loss=aggregate_value_for_modes(
                    runs_by_tm, tid, BUNDLE_MODES, lambda run: run.total_loss_pct()
                ),
            )
        )

    metric_defs = [
        ("Rate send avg", "rate_send_avg", "ms", 1000.0),
        ("Rate send jitter", "rate_send_jitter", "ms", 1000.0),
        ("Rate interval jitter", "rate_itvl_jitter", "ms", 1000.0),
        ("Rate loss", "rate_loss", "%", 1.0),
        ("Payload send avg", "payload_send_avg", "ms", 1000.0),
        ("Payload loss", "payload_loss", "%", 1.0),
        ("Multi per-addr avg", "multi_per_addr_avg", "ms", 1.0),
        ("Multi loss", "multi_loss", "%", 1.0),
        ("Bundle send avg", "bundle_send_avg", "ms", 1000.0),
        ("Bundle loss", "bundle_loss", "%", 1.0),
    ]
    ramp_metric_defs = [
        ("Ramp rate breakpoint", 11),
        ("Ramp payload breakpoint", 12),
        ("Ramp multi breakpoint", 13),
        ("Ramp bundle breakpoint", 14),
    ]

    wins = defaultdict(float)
    any_metric = False

    subheader("Metric Winners")
    for label, key, unit, scale in metric_defs:
        winners = metric_winners(rows, key)
        if not winners:
            print(f"  {label:<20}  —")
            continue
        any_metric = True
        share = 1.0 / len(winners)
        for winner in winners:
            wins[winner["name"]] += share
        names = " = ".join(winner["name"] for winner in winners)
        value = winners[0][key]
        shown = value / scale if scale != 1.0 else value
        print(f"  {label:<20}  {names}  ({shown:.3f} {unit})")

    for label, mode in ramp_metric_defs:
        ramp_rows = []
        for tid in OSC_TIDS:
            ramp_rows.append(
                dict(
                    name=TRANSPORT[tid],
                    score=ramp_survival_score(mode, runs_by_tm.get((tid, mode), [])),
                )
            )
        winners = metric_winners_high(ramp_rows, "score")
        if not winners:
            print(f"  {label:<20}  —")
            continue
        any_metric = True
        share = 1.0 / len(winners)
        for winner in winners:
            wins[winner["name"]] += share
        names = " = ".join(winner["name"] for winner in winners)
        print(f"  {label:<20}  {names}")

    if not any_metric:
        print("  No OSC-library metrics found.")
        return

    subheader("Overall Score  [shared wins]")
    scoreboard = sorted(
        (dict(name=row["name"], score=wins.get(row["name"], 0.0)) for row in rows),
        key=lambda item: (-item["score"], item["name"]),
    )
    for row in scoreboard:
        print(f"  {row['name']:<10}  {row['score']:.3f}")

    best_score = scoreboard[0]["score"]
    best = [row["name"] for row in scoreboard if abs(row["score"] - best_score) <= 1e-9]
    print()
    if len(best) == 1:
        print(f"  Best overall: {best[0]}")
    else:
        print(f"  Best overall: {' = '.join(best)}")


# ── Metric conversion and breakpoint helpers ─────────────────────────────────
def convert_stats_to_ms(metric):
    """Convert microsecond-based stats dicts to milliseconds for printing."""
    if metric is None:
        return None
    return {key: (value / 1000 if key != "count" else value) for key, value in metric.items()}


def to_ms(value):
    return value / 1000 if value is not None else None


def wifi_brief(run):
    """Compact WiFi label for ramp tables.
    """
    if run.wifi_event_count:
        return f"events:{run.wifi_event_count}"
    if run.wifi_start and run.wifi_end:
        return f"{run.wifi_start['rssi']}→{run.wifi_end['rssi']} dBm"
    if run.wifi_start:
        return f"{run.wifi_start['rssi']} dBm"
    return "—"


def ramp_cost_limit_ms(mode, run):
    """Return the send-cost limit that defines a ramp cost breakpoint."""
    if mode == 11 and run.interval_us:
        return (run.interval_us / 1000.0) * RAMP_COST_RATIO_LIMIT
    return RAMP_ABS_COST_LIMIT_MS


def ramp_stop_reason(mode, run):
    """Return the first applicable stop reason for a ramp step.
    """
    attempted = run.attempted_packets()
    if not attempted:
        return "incomplete"

    loss_pct = run.total_loss_pct()
    avg_send_ms = to_ms(run.send_cost_stats()["avg"])
    cost_limit_ms = ramp_cost_limit_ms(mode, run)

    hit_loss = loss_pct > RAMP_ERROR_RATE_LIMIT or loss_pct > LOSS_BREAKPOINT_PCT
    hit_cost = avg_send_ms is not None and avg_send_ms > cost_limit_ms

    if hit_cost and hit_loss:
        return "cost+loss"
    if hit_cost:
        return "cost"
    if hit_loss:
        return "loss"
    return None


# ── Main analysis entrypoint ─────────────────────────────────────────────────
def analyze(serial_path, csv_path):
    print(f"\nReading {serial_path} and {csv_path} ...")
    all_data = parse_serial(serial_path)
    all_data = parse_csv(csv_path, all_data)

    if not all_data:
        print("No data found. Check file paths.")
        sys.exit(1)

    runs_by_tm = group_by_tm(all_data)

    print(
        f"Loaded {len(all_data)} runs across "
        f"{len(set(run.mode for run in all_data.values() if run.mode is not None))} modes and "
        f"{len(set(run.transport for run in all_data.values() if run.transport is not None))} transports."
    )

    print_fixed_mode_sections(runs_by_tm)
    print_ramp_sections(runs_by_tm)
    print_verdict(runs_by_tm)
    print(f"\n{'═' * WIDTH}\n")


# ── Entry point ───────────────────────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Analyze OSC benchmark results offline.")
    parser.add_argument("--serial", default="serial_log.txt")
    parser.add_argument("--csv", default="benchmark_packets.csv")
    args = parser.parse_args()
    analyze(args.serial, args.csv)
