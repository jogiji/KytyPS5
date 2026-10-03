#!/usr/bin/env python3
"""Stream a Kyty GPU-gap trace CSV and summarize event and ring-drop counts."""

from __future__ import annotations

import argparse
import collections
import csv
import json
from pathlib import Path


def analyze(path: Path) -> dict[str, object]:
    event_counts: collections.Counter[str] = collections.Counter()
    thread_events: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    ring_blocks: list[dict[str, object]] = []
    block_events: collections.Counter[str] = collections.Counter()
    block_threads: set[str] = set()
    calibration_deviations: list[int] = []
    calibration_failures = 0
    gpu_gap_durations_ns: list[int] = []
    rows = 0

    with path.open("r", newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        for row in reader:
            event = row["event"]
            thread_id = row["thread_id"]
            value0 = int(row["value0"])
            value1 = int(row["value1"])
            rows += 1
            event_counts[event] += 1

            if event == "ring_dropped":
                ring_blocks.append(
                    {
                        "thread_ids": sorted(block_threads),
                        "records_written": sum(block_events.values()),
                        "event_counts": dict(block_events),
                        "dropped_total": value0,
                    }
                )
                block_events.clear()
                block_threads.clear()
                continue

            block_events[event] += 1
            block_threads.add(thread_id)
            thread_events[thread_id][event] += 1
            if event == "clock_calibration":
                calibration_deviations.append(value1)
            elif event == "clock_calibration_failure":
                calibration_failures += 1
            elif event == "gpu_gap":
                gpu_gap_durations_ns.append(value0)

    dropped_blocks = [block for block in ring_blocks if block["dropped_total"] > 0]
    return {
        "path": str(path),
        "rows": rows,
        "event_counts": event_counts.most_common(),
        "thread_events": {
            thread: counts.most_common() for thread, counts in thread_events.items()
        },
        "ring_drop_blocks": len(dropped_blocks),
        "ring_drop_max_cumulative": max(
            (int(block["dropped_total"]) for block in ring_blocks), default=0
        ),
        "ring_drop_examples": dropped_blocks[-12:],
        "calibrations": len(calibration_deviations),
        "calibration_max_deviation_ns": max(calibration_deviations, default=0),
        "calibration_failures": calibration_failures,
        "gpu_gap_events": len(gpu_gap_durations_ns),
        "gpu_gap_mean_ms": (
            sum(gpu_gap_durations_ns) / len(gpu_gap_durations_ns) / 1e6
            if gpu_gap_durations_ns
            else None
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.csv), indent=2))


if __name__ == "__main__":
    main()
