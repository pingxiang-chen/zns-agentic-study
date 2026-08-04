#!/usr/bin/env python3
"""Calculate deltas for matching before/after CSV snapshots.

By default this scans results/hw for files named:

    <prefix>_before.csv
    <prefix>_after.csv

and prints a CSV containing numeric after-minus-before differences using the
same columns as the source CSVs.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import OrderedDict
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Iterable


DEFAULT_BEFORE_SUFFIX = "_before.csv"
DEFAULT_AFTER_SUFFIX = "_after.csv"


@dataclass(frozen=True)
class CsvPair:
    prefix: Path
    before: Path
    after: Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Find matching before/after CSV files and write after-minus-before "
            "deltas as CSV."
        )
    )
    parser.add_argument(
        "targets",
        nargs="*",
        default=["../results/hw/"],
        help=(
            "Directory to scan, a *_before.csv or *_after.csv file, or a shared "
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
        "--long",
        action="store_true",
        help="Emit one row per field with before, after, and delta values.",
    )
    parser.add_argument(
        "--include-metadata",
        action="store_true",
        help=(
            "In wide output, include before_file and after_file columns before "
            "the original CSV columns."
        ),
    )
    return parser.parse_args()


def strip_suffix(path: Path, suffix: str) -> Path:
    name = path.name
    if not name.endswith(suffix):
        raise ValueError(f"{path} does not end with {suffix!r}")
    return path.with_name(name[: -len(suffix)])


def pair_from_prefix(prefix: Path, before_suffix: str, after_suffix: str) -> CsvPair:
    return CsvPair(
        prefix=prefix,
        before=prefix.with_name(prefix.name + before_suffix),
        after=prefix.with_name(prefix.name + after_suffix),
    )


def find_pairs(
    targets: Iterable[str],
    before_suffix: str,
    after_suffix: str,
) -> list[CsvPair]:
    pairs: OrderedDict[Path, CsvPair] = OrderedDict()

    for target_text in targets:
        target = Path(target_text)
        candidates: list[Path]

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


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no CSV header")
        return reader.fieldnames, list(reader)


def as_decimal(value: str) -> Decimal | None:
    value = value.strip()
    if value == "":
        return None
    try:
        return Decimal(value)
    except InvalidOperation:
        return None


def format_decimal(value: Decimal) -> str:
    if value == value.to_integral_value():
        return str(value.quantize(Decimal(1)))
    return format(value.normalize(), "f")


def diff_value(before: str, after: str) -> str:
    before_num = as_decimal(before)
    after_num = as_decimal(after)

    if before_num is not None and after_num is not None:
        return format_decimal(after_num - before_num)
    if before == after:
        return ""
    return f"{before} -> {after}"


def row_label(pair: CsvPair) -> str:
    return pair.prefix.as_posix()


def build_wide_rows(
    pairs: list[CsvPair],
    include_metadata: bool,
) -> tuple[list[str], list[dict[str, str]]]:
    rows: list[dict[str, str]] = []
    delta_fields: list[str] = []

    for pair in pairs:
        before_fields, before_rows = read_csv(pair.before)
        after_fields, after_rows = read_csv(pair.after)

        if before_fields != after_fields:
            raise ValueError(
                f"header mismatch for {pair.prefix}: "
                f"{pair.before} fields differ from {pair.after}"
            )
        if len(before_rows) != len(after_rows):
            raise ValueError(
                f"row count mismatch for {pair.prefix}: "
                f"{pair.before} has {len(before_rows)} row(s), "
                f"{pair.after} has {len(after_rows)} row(s)"
            )

        for field in before_fields:
            if field not in delta_fields:
                delta_fields.append(field)

        for index, (before_row, after_row) in enumerate(zip(before_rows, after_rows)):
            row = {}
            if include_metadata:
                row.update(
                    {
                        "before_file": pair.before.as_posix(),
                        "after_file": pair.after.as_posix(),
                    }
                )
            for field in before_fields:
                row[field] = diff_value(before_row.get(field, ""), after_row.get(field, ""))
            rows.append(row)

    metadata_fields = ["before_file", "after_file"]
    if include_metadata:
        return [*metadata_fields, *delta_fields], rows
    return delta_fields, rows


def build_long_rows(pairs: list[CsvPair]) -> tuple[list[str], list[dict[str, str]]]:
    rows: list[dict[str, str]] = []

    for pair in pairs:
        before_fields, before_rows = read_csv(pair.before)
        after_fields, after_rows = read_csv(pair.after)

        if before_fields != after_fields:
            raise ValueError(
                f"header mismatch for {pair.prefix}: "
                f"{pair.before} fields differ from {pair.after}"
            )
        if len(before_rows) != len(after_rows):
            raise ValueError(
                f"row count mismatch for {pair.prefix}: "
                f"{pair.before} has {len(before_rows)} row(s), "
                f"{pair.after} has {len(after_rows)} row(s)"
            )

        for index, (before_row, after_row) in enumerate(zip(before_rows, after_rows)):
            for field in before_fields:
                before = before_row.get(field, "")
                after = after_row.get(field, "")
                rows.append(
                    {
                        "prefix": row_label(pair),
                        "row_index": str(index),
                        "field": field,
                        "before": before,
                        "after": after,
                        "delta": diff_value(before, after),
                        "before_file": pair.before.as_posix(),
                        "after_file": pair.after.as_posix(),
                    }
                )

    return [
        "prefix",
        "row_index",
        "field",
        "before",
        "after",
        "delta",
        "before_file",
        "after_file",
    ], rows


def validate_pairs(pairs: list[CsvPair]) -> None:
    if not pairs:
        raise FileNotFoundError("no before/after CSV pairs found")

    missing = [
        str(path)
        for pair in pairs
        for path in (pair.before, pair.after)
        if not path.is_file()
    ]
    if missing:
        raise FileNotFoundError("missing expected CSV file(s): " + ", ".join(missing))


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

    try:
        pairs = find_pairs(args.targets, args.before_suffix, args.after_suffix)
        validate_pairs(pairs)
        if args.long:
            fieldnames, rows = build_long_rows(pairs)
        else:
            fieldnames, rows = build_wide_rows(pairs, args.include_metadata)
        write_csv(fieldnames, rows, args.output)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
