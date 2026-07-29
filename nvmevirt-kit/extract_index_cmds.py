#!/usr/bin/env python3
"""Extract index read/write latency rows from nvmevirt summary files."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


COMMANDS = {"index_write_cmd", "index_read_cmd", "kv_write_op", "kv_read_op"}
COMMAND_ORDER = {"index_write_cmd": 0, "index_read_cmd": 1, "kv_write_op" :2, "kv_read_op" :3}
SUMMARY_GLOB = "*_summary.txt"

LATENCY_RE = re.compile(r"^LAT\s+(?P<cmd>\S+)\s+(?P<fields>.+)$")
FIELD_RE = re.compile(r"(?P<key>[A-Za-z0-9_]+)=(?P<value>\S+)")
ZNS_RE = re.compile(r"(?:^|_)zns(?:_k?(?P<k>[0-9]+))?(?:_|$)")
CONV_RE = re.compile(r"(?:^|_)conv(?:_|$)")


def infer_config(path: Path) -> tuple[str, str]:
    """Infer mode and ZNS stripe-write k from the summary filename."""
    stem = path.stem

    zns_match = ZNS_RE.search(stem)
    if zns_match:
        return "zns", zns_match.group("k") or ""

    if CONV_RE.search(stem):
        return "conv", ""

    return "unknown", ""


def parse_latency_line(line: str) -> tuple[str, dict[str, str]] | None:
    match = LATENCY_RE.match(line.strip())
    if not match:
        return None

    command = match.group("cmd")
    if command not in COMMANDS:
        return None

    fields = {
        field_match.group("key"): field_match.group("value")
        for field_match in FIELD_RE.finditer(match.group("fields"))
    }
    return command, fields


def collect_rows(summary_paths: list[Path]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []

    for path in summary_paths:
        mode, k = infer_config(path)

        with path.open("r", encoding="utf-8") as summary_file:
            for line in summary_file:
                parsed = parse_latency_line(line)
                if parsed is None:
                    continue

                command, fields = parsed
                rows.append(
                    {
                        "file": path.name,
                        "mode": mode,
                        "k": k,
                        "command": command,
                        **fields,
                    }
                )

    return rows


def sort_key(path: Path) -> tuple[int, int, str]:
    mode, k = infer_config(path)
    if mode == "conv":
        return (0, 0, path.name)
    if mode == "zns":
        return (1, int(k) if k else 0, path.name)
    return (2, 0, path.name)


def row_sort_key(row: dict[str, str]) -> tuple[int, int, int, str]:
    mode_order = {"conv": 0, "zns": 1}.get(row["mode"], 2)
    k = int(row["k"]) if row["k"] else 0
    return (COMMAND_ORDER[row["command"]], mode_order, k, row["file"])


def write_csv(rows: list[dict[str, str]], output) -> None:
    fieldnames = [
        "file",
        "mode",
        "k",
        "command",
        "n",
        "mean_us",
        "p50_us",
        "p90_us",
        "p99_us",
        "p999_us",
        "max_us",
    ]

    writer = csv.DictWriter(output, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def write_text(rows: list[dict[str, str]], output) -> None:
    for row in rows:
        label = "conv" if row["mode"] == "conv" else f"zns_k{row['k']}"
        metrics = " ".join(
            f"{key}={row[key]}"
            for key in ("n", "mean_us", "p50_us", "p90_us", "p99_us", "p999_us", "max_us")
            if key in row
        )
        print(f"{label:8s} {row['command']:15s} {metrics}  # {row['file']}", file=output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract LAT index_write_cmd and index_read_cmd rows from *_summary.txt files."
    )
    parser.add_argument(
        "summary_dir",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory containing *_summary.txt files. Defaults to this script's directory.",
    )
    parser.add_argument(
        "--csv",
        action="store_true",
        help="Print CSV instead of aligned text.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Write output to this file instead of stdout.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary_dir = args.summary_dir.resolve()

    if not summary_dir.is_dir():
        print(f"error: summary directory does not exist: {summary_dir}", file=sys.stderr)
        return 1

    summary_paths = sorted(summary_dir.glob(SUMMARY_GLOB), key=sort_key)
    if not summary_paths:
        print(f"error: no {SUMMARY_GLOB} files found in {summary_dir}", file=sys.stderr)
        return 1

    rows = collect_rows(summary_paths)
    if not rows:
        print(
            f"error: no index_write_cmd or index_read_cmd rows found in {summary_dir}",
            file=sys.stderr,
        )
        return 1

    rows.sort(key=row_sort_key)

    if args.output:
        with args.output.open("w", encoding="utf-8", newline="") as output:
            if args.csv:
                write_csv(rows, output)
            else:
                write_text(rows, output)
    elif args.csv:
        write_csv(rows, sys.stdout)
    else:
        write_text(rows, sys.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
