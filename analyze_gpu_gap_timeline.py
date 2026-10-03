#!/usr/bin/env python3
"""Correlate calibrated GPU idle gaps with Kyty producer and command-stream spans."""

from __future__ import annotations

import argparse
import collections
import csv
import json
import statistics
from pathlib import Path


SPAN_PAIRS = {
    "thread_gpu_idle_begin": ("thread_gpu_idle_end", "thread_gpu_idle"),
    "thread_gpu_blocked_begin": ("thread_gpu_blocked_end", "thread_gpu_blocked"),
    "thread_gpu_process_begin": ("thread_gpu_process_end", "thread_gpu_process"),
    "stream_empty_begin": ("stream_empty_end", "command_stream_empty"),
    "stream_consume_end": ("stream_consume_begin", "command_stream_consumer_idle"),
    "record_batch_begin": ("record_batch_end", "record_batch"),
    "explicit_semaphore_wait_begin": (
        "explicit_semaphore_wait_end",
        "explicit_semaphore_wait",
    ),
    "submit_begin": ("submit_end", "vulkan_submit"),
    "pm4_begin": ("pm4_end", "pm4"),
}


def load(path: Path):
    events = []
    counts: collections.Counter[str] = collections.Counter()
    dropped = 0
    calibrations = []
    failures = 0
    with path.open("r", newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            event = row["event"]
            tid = row["thread_id"]
            ts = int(row["timestamp_ns"])
            v0 = int(row["value0"])
            v1 = int(row["value1"])
            counts[event] += 1
            if event == "ring_dropped":
                dropped = max(dropped, v0)
            elif event == "clock_calibration":
                calibrations.append(v1)
            elif event == "clock_calibration_failure":
                failures += 1
            else:
                events.append((ts, event, tid, v0, v1))
    events.sort(key=lambda e: e[0])
    return events, counts, dropped, calibrations, failures


def pair_spans(events):
    pending = {}
    spans: dict[str, list[tuple[int, int, str, int, int]]] = collections.defaultdict(list)
    reverse = {end: (begin, name) for begin, (end, name) in SPAN_PAIRS.items()}
    for ts, event, tid, v0, v1 in events:
        if event in SPAN_PAIRS:
            end, name = SPAN_PAIRS[event]
            pending.setdefault((tid, event), []).append((ts, v0, v1, name))
        elif event in reverse:
            begin, name = reverse[event]
            stack = pending.get((tid, begin))
            if stack:
                start, v0, v1, _ = stack.pop()
                if ts >= start:
                    spans[name].append((start, ts, tid, v0, v1))
    return spans


def state_update(state, event):
    _ts, name, _tid, v0, v1 = event
    if name in ("producer_enqueue", "producer_dequeue"):
        state["guest_submission_depth"] = v0
    elif name in ("producer_command_enqueue", "producer_command_dequeue"):
        state["guest_command_depth"] = v0
    elif name == "stream_depth_sample":
        state["command_stream_bytes"] = v0
        state["record_packets"] = v1
    elif name == "submit_queue_sample":
        state["submit_jobs"] = v0
        state["submit_packets"] = v1


def overlap_ms_by_gap(gaps, spans, same_gpu_thread=False):
    groups = collections.defaultdict(list)
    for start, end, tid, *_ in spans:
        groups[tid if same_gpu_thread else None].append((start, end))
    merged = []
    for tid, intervals in groups.items():
        thread_merged = []
        for start, end in sorted(intervals):
            if thread_merged and start <= thread_merged[-1][1]:
                thread_merged[-1] = (thread_merged[-1][0], max(thread_merged[-1][1], end))
            else:
                thread_merged.append((start, end))
        merged.extend((start, end, tid) for start, end in thread_merged)
    merged.sort()
    result = [0] * len(gaps)
    index = 0
    for gap_index, gap in enumerate(gaps):
        start = gap["start_ns"]
        end = start + int(gap["duration_ms"] * 1e6)
        while index < len(merged) and merged[index][1] <= start:
            index += 1
        cursor = index
        while cursor < len(merged) and merged[cursor][0] < end:
            a, b, tid = merged[cursor]
            if not same_gpu_thread or tid == gap["gpu_gap_thread"]:
                result[gap_index] += max(0, min(end, b) - max(start, a))
            cursor += 1
    return result


def empty_state():
    return {
        "guest_submission_depth": None,
        "guest_command_depth": None,
        "command_stream_bytes": None,
        "record_packets": None,
        "submit_jobs": None,
        "submit_packets": None,
    }


def analyze(
    path: Path, threshold_ms: float, detail_count: int,
    window_start_ns: int | None, window_end_ns: int | None,
):
    events, counts, dropped, calibrations, failures = load(path)
    spans = pair_spans(events)
    roles = {tid: v0 for _ts, event, tid, v0, _v1 in events if event == "thread_role"}
    thread_gpu_ids = {tid for tid, role in roles.items() if role == 1}
    gap_rows = [e for e in events if e[1] == "gpu_gap"]
    gaps = []
    for ts, _event, tid, duration, uncertainty in gap_rows:
        if window_start_ns is not None and ts < window_start_ns:
            continue
        if window_end_ns is not None and ts + duration > window_end_ns:
            continue
        if duration < threshold_ms * 1_000_000:
            continue
        end = ts + duration
        gaps.append(
            {
                "start_ns": ts,
                "duration_ms": round(duration / 1e6, 4),
                "uncertainty_us": round(uncertainty / 1e3, 3),
                "gpu_gap_thread": tid,
                "overlap_ms": {},
                "state_at_start": {},
            }
        )
    for name, items in spans.items():
        values = overlap_ms_by_gap(
            gaps, items,
            same_gpu_thread=name in {
                "thread_gpu_idle", "thread_gpu_blocked", "thread_gpu_process",
                "explicit_semaphore_wait", "pm4",
            },
        )
        for gap, value in zip(gaps, values):
            if value > 0:
                gap["overlap_ms"][name] = round(value / 1e6, 4)
    state = empty_state()
    event_index = 0
    for gap in gaps:
        while event_index < len(events) and events[event_index][0] <= gap["start_ns"]:
            state_update(state, events[event_index])
            event_index += 1
        gap["state_at_start"] = state.copy()
    gaps.sort(key=lambda g: g["duration_ms"], reverse=True)
    overlap_totals = collections.Counter()
    for gap in gaps:
        for name, ms in gap["overlap_ms"].items():
            overlap_totals[name] += ms
    gap_durations = [gap["duration_ms"] for gap in gaps]
    window_waits = []
    ticks = collections.defaultdict(lambda: {"count": 0, "total_ms": 0.0, "max_ms": 0.0})
    for start, end, tid, tick, _v1 in spans.get("explicit_semaphore_wait", ()):
        if window_start_ns is not None and end <= window_start_ns:
            continue
        if window_end_ns is not None and start >= window_end_ns:
            continue
        clipped_ms = (min(end, window_end_ns or end) - max(start, window_start_ns or start)) / 1e6
        if clipped_ms <= 0:
            continue
        window_waits.append((clipped_ms, tid, tick))
        entry = ticks[(tid, str(tick))]
        entry["count"] += 1
        entry["total_ms"] += clipped_ms
        entry["max_ms"] = max(entry["max_ms"], clipped_ms)
    gpu_thread_waits = [w for w in window_waits if w[1] in thread_gpu_ids]
    sem_overlap = overlap_totals.get("explicit_semaphore_wait", 0.0)
    state_counts = {}
    for field, predicate in (
        ("guest_submission_depth", lambda value: value is not None and value > 0),
        ("guest_command_depth", lambda value: value is not None and value > 0),
        ("command_stream_bytes", lambda value: value is not None and value > 0),
        ("submit_jobs", lambda value: value is not None and value > 0),
        ("submit_packets", lambda value: value is not None and value > 0),
    ):
        state_counts[field + "_positive"] = sum(
            1 for gap in gaps if predicate(gap["state_at_start"].get(field))
        )
    overlap_gap_counts = {
        name: sum(1 for gap in gaps if gap["overlap_ms"].get(name, 0) > 0)
        for name in overlap_totals
    }
    summary = {
        "path": str(path),
        "window_start_ns": window_start_ns,
        "window_end_ns": window_end_ns,
        "gpu_gap_events_total": counts["gpu_gap"],
        "gpu_gap_events_over_threshold": len(gaps),
        "threshold_ms": threshold_ms,
        "gap_duration_total_ms": round(sum(gap_durations), 3),
        "gap_duration_mean_ms": round(statistics.mean(gap_durations), 4) if gaps else None,
        "gap_duration_median_ms": round(statistics.median(gap_durations), 4) if gaps else None,
        "gap_duration_p95_ms": round(
            statistics.quantiles(gap_durations, n=20)[18], 4
        ) if len(gap_durations) >= 20 else None,
        "ring_dropped_max_cumulative": dropped,
        "clock_calibrations": len(calibrations),
        "calibration_max_deviation_us": round(max(calibrations, default=0) / 1e3, 3),
        "calibration_failures": failures,
        "thread_gpu_role_ids": sorted(thread_gpu_ids),
        "span_counts": {name: len(items) for name, items in spans.items()},
        "explicit_semaphore_waits_in_window_all_threads": len(window_waits),
        "explicit_semaphore_wait_ms_in_window_all_threads": round(
            sum(w[0] for w in window_waits), 3
        ),
        "explicit_semaphore_waits_on_thread_gpu": len(gpu_thread_waits),
        "explicit_semaphore_wait_ms_on_thread_gpu": round(
            sum(w[0] for w in gpu_thread_waits), 3
        ),
        "explicit_wait_overlap_with_selected_gaps_pct": round(
            sem_overlap / sum(gap_durations) * 100.0, 2
        ) if gap_durations else None,
        "longest_semaphore_waits_ms": [
            {"duration_ms": round(w[0], 4), "thread_id": w[1], "tick": w[2]}
            for w in sorted(window_waits, reverse=True)[:10]
        ],
        "top_semaphore_ticks": [
            {"thread_id": tid, "role": roles.get(tid), "tick": tick,
             "count": values["count"],
             "total_ms": round(values["total_ms"], 3),
             "max_ms": round(values["max_ms"], 3)}
            for (tid, tick), values in sorted(
                ticks.items(), key=lambda item: item[1]["total_ms"], reverse=True
            )[:10]
        ],
        "selected_gap_queue_state_positive_counts": state_counts,
        "selected_gap_overlap_counts": overlap_gap_counts,
        "overlap_totals_ms_nonexclusive": {
            name: round(value, 3) for name, value in overlap_totals.most_common()
        },
        "longest_gaps": gaps[:detail_count],
    }
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--threshold-ms", type=float, default=5.0)
    parser.add_argument("--details", type=int, default=20)
    parser.add_argument("--start-ns", type=int)
    parser.add_argument("--end-ns", type=int)
    args = parser.parse_args()
    print(json.dumps(
        analyze(args.csv, args.threshold_ms, args.details, args.start_ns, args.end_ns),
        indent=2,
    ))


if __name__ == "__main__":
    main()
