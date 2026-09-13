#!/usr/bin/env python3
"""Extract cumulative GC series from conventional and ZNS SSD text logs.

Usage:
    python3 generate_gc_tikz_data.py CONVENTIONAL_LOG ZNS_LOG [OUTPUT_DIR]

The output files are consumed by cumulative_gc_events_tikz.tex.
"""

from __future__ import annotations

import re
import sys
from datetime import datetime
from pathlib import Path


KEY_VALUE = re.compile(r"([A-Za-z_]+)=([^\s]+)")


def fields(line: str) -> dict[str, str]:
    return dict(KEY_VALUE.findall(line))


def parse_conventional(path: Path) -> list[tuple[float, int]]:
    points: list[tuple[float, int]] = []
    pending_timestamp: datetime | None = None
    start: datetime | None = None

    for line in path.read_text().splitlines():
        if line.startswith("timestamp="):
            pending_timestamp = datetime.strptime(
                line.removeprefix("timestamp="), "%Y-%m-%d:%H:%M:%S"
            )
            if start is None:
                start = pending_timestamp
        elif line.startswith("conv_ftl ") and pending_timestamp is not None:
            values = fields(line)
            elapsed_min = (pending_timestamp - start).total_seconds() / 60.0
            points.append((elapsed_min, int(values["gc_count"])))
            pending_timestamp = None

    if not points:
        raise ValueError(f"No conventional FTL records found in {path}")
    return points


def parse_zns(path: Path) -> list[tuple[float, int]]:
    points: list[tuple[float, int]] = []
    prefix = "ZNS_HOST_MANAGEMENT_GC_EVENT[interval]"

    for line in path.read_text().splitlines():
        if not line.startswith(prefix):
            continue
        values = fields(line)
        points.append(
            (
                float(values["elapsed_s"]) / 60.0,
                int(values["total_run_host_gc_count"]),
            )
        )

    if not points:
        raise ValueError(f"No ZNS host-management GC records found in {path}")
    return points


def write_dat(path: Path, points: list[tuple[float, int]]) -> None:
    # A logarithmic y-axis cannot display zero, so retain the elapsed gap by
    # starting each plotted series at its first positive cumulative count.
    positive = [(elapsed, count) for elapsed, count in points if count > 0]
    if not positive:
        raise ValueError(f"No positive cumulative GC counts found for {path.name}")

    rows = ["elapsed_min cumulative_gc"]
    rows.extend(f"{elapsed:.6f} {count}" for elapsed, count in positive)
    path.write_text("\n".join(rows) + "\n")


def main() -> None:
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "Usage: python3 generate_gc_tikz_data.py "
            "CONVENTIONAL_LOG ZNS_LOG [OUTPUT_DIR]"
        )

    conventional_log = Path(sys.argv[1]).expanduser().resolve()
    zns_log = Path(sys.argv[2]).expanduser().resolve()
    output_dir = (
        Path(sys.argv[3]).expanduser().resolve()
        if len(sys.argv) == 4
        else Path(__file__).resolve().parent
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    conventional = parse_conventional(conventional_log)
    zns = parse_zns(zns_log)
    write_dat(output_dir / "conventional_gc_cumulative.dat", conventional)
    write_dat(output_dir / "zns_gc_cumulative.dat", zns)

    print(
        f"Conventional: {conventional[-1][1]:,} cumulative GC events; "
        f"ZNS: {zns[-1][1]:,} cumulative host GC events"
    )


if __name__ == "__main__":
    main()