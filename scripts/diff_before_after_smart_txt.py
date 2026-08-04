#!/usr/bin/env python3
"""Calculate deltas for selected counters in before/after SMART text logs."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_BEFORE_SUFFIX = "_before.txt"
DEFAULT_AFTER_SUFFIX = "_after.txt"
DEFAULT_FIELDS = [
    "Data Units Read",
    "Data Units Written",
    "host_read_commands",
    "host_write_commands",
]
INTEGER_RE = re.compile(r"[-+]?\d[\d,]*")


@dataclass(frozen=True)
class TextPair:
    prefix: Path
    before: Path
    after: Path


def default_results_dir() -> str:
    return str(Path(__file__).resolve().parents[1] / "results" / "hw")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Find matching before/after SMART text logs and write selected "
            "after-minus-before deltas as CSV."
        )
    )
    parser.add_argument(
        "targets",
        nargs="*",
        default=[default_results_dir()],
        help=(
            "Directory to scan, a *_before.txt or *_after.txt file, or a shared "
            "file prefix. Defaults to results/hw."
        ),
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Write output CSV here instead of stdout.",
    )
    parser.add_argument(
        "--before-suffix",
        default=DEFAULT_BEFORE_SUFFIX,
        help=f"Suffix for before files. Default: {DEFAULT_BEFORE_SUFFIX}",
    )
    parser.add_argument(
        "--after-suffix",
        default=DEFAULT_AFTER_SUFFIX,
        help=f"Suffix for after files. Default: {DEFAULT_AFTER_SUFFIX}",
    )
    parser.add_argument(
        "--field",
        dest="fields",
        action="append",
        help=(
            "Counter field to diff. Repeat to override the default field list. "
            "Default fields: " + ", ".join(DEFAULT_FIELDS)
        ),
    )
    parser.add_argument(
        "--include-metadata",
        action="store_true",
        help=(
            "Include before_file and after_file columns before the counter "
            "delta columns."
        ),
    )
    return parser.parse_args()


def strip_suffix(path: Path, suffix: str) -> Path:
    name = path.name
    if not name.endswith(suffix):
        raise ValueError(f"{path} does not end with {suffix!r}")
    return path.with_name(name[: -len(suffix)])


def pair_from_prefix(prefix: Path, before_suffix: str, after_suffix: str) -> TextPair:
    return TextPair(
        prefix=prefix,
        before=prefix.with_name(prefix.name + before_suffix),
        after=prefix.with_name(prefix.name + after_suffix),
    )


def find_pairs(
    targets: Iterable[str],
    before_suffix: str,
    after_suffix: str,
) -> list[TextPair]:
    pairs: OrderedDict[Path, TextPair] = OrderedDict()

    for target_text in targets:
        target = Path(target_text)
        if target.is_dir():
            candidates = sorted(target.glob(f"*{before_suffix}"))
        else:
            candidates = [target]

        for candidate in candidates:
            if candidate.name.endswith(before_suffix):
                prefix = strip_suffix(candidate, before_suffix)
            elif candidate.name.endswith(after_suffix):
                prefix = strip_suffix(candidate, after_suffix)
            else:
                prefix = candidate

            pair = pair_from_prefix(prefix, before_suffix, after_suffix)
            pairs[pair.prefix] = pair

    return list(pairs.values())


def parse_counter_value(field: str, raw_value: str, path: Path) -> int:
    match = INTEGER_RE.search(raw_value)
    if match is None:
        raise ValueError(f"{path}: field {field!r} has no integer value: {raw_value!r}")
    return int(match.group(0).replace(",", ""))


def read_counters(path: Path, fields: list[str]) -> dict[str, int]:
    wanted = set(fields)
    counters: dict[str, int] = {}

    with path.open() as handle:
        for line in handle:
            if ":" not in line:
                continue

            key, raw_value = line.split(":", 1)
            key = key.strip()
            if key in wanted:
                counters[key] = parse_counter_value(key, raw_value, path)

    missing = [field for field in fields if field not in counters]
    if missing:
        raise ValueError(f"{path}: missing field(s): {', '.join(missing)}")

    return counters


def validate_pairs(pairs: list[TextPair]) -> None:
    if not pairs:
        raise FileNotFoundError("no before/after text pairs found")

    missing = [
        str(path)
        for pair in pairs
        for path in (pair.before, pair.after)
        if not path.is_file()
    ]
    if missing:
        raise FileNotFoundError("missing expected text file(s): " + ", ".join(missing))


def build_rows(
    pairs: list[TextPair],
    fields: list[str],
    include_metadata: bool,
) -> tuple[list[str], list[dict[str, str]]]:
    rows: list[dict[str, str]] = []

    for pair in pairs:
        before = read_counters(pair.before, fields)
        after = read_counters(pair.after, fields)

        row = {}
        if include_metadata:
            row.update(
                {
                    "before_file": pair.before.as_posix(),
                    "after_file": pair.after.as_posix(),
                }
            )

        for field in fields:
            row[field] = str(after[field] - before[field])
        rows.append(row)

    if include_metadata:
        return ["before_file", "after_file", *fields], rows
    return fields, rows


def write_csv(
    fieldnames: list[str],
    rows: list[dict[str, str]],
    output: Path | None,
) -> None:
    if output is None:
        writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
        return

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    fields = args.fields if args.fields else DEFAULT_FIELDS

    try:
        pairs = find_pairs(args.targets, args.before_suffix, args.after_suffix)
        validate_pairs(pairs)
        fieldnames, rows = build_rows(pairs, fields, args.include_metadata)
        write_csv(fieldnames, rows, args.output)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
