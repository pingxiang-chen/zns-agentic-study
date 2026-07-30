#!/usr/bin/env python3

import argparse
import math
from decimal import Decimal, InvalidOperation
from pathlib import Path

NS_PER_SECOND = Decimal("1000000000")


def parse_percentage(value: str) -> float:
    """Convert 10, 10%, or 0.1 into a fraction between 0 and 1."""
    value = value.strip()

    try:
        if value.endswith("%"):
            fraction = float(value[:-1]) / 100.0
        else:
            number = float(value)
            fraction = number / 100.0 if number > 1 else number
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"Invalid percentage: {value!r}. Use values such as 10%, 10, or 0.1."
        ) from exc

    if not 0 <= fraction <= 1:
        raise argparse.ArgumentTypeError(
            "Percentage must be between 0% and 100%."
        )

    return fraction


def parse_measure_start_seconds(value: str) -> int:
    """Convert a nonnegative seconds value into whole nanoseconds."""
    try:
        seconds = Decimal(value.strip())
    except InvalidOperation as exc:
        raise argparse.ArgumentTypeError(
            f"Invalid measurement start time: {value!r}. Use seconds, such as 10 or 10.5."
        ) from exc

    if not seconds.is_finite() or seconds < 0:
        raise argparse.ArgumentTypeError(
            "Measurement start time must be a nonnegative number of seconds."
        )

    nanoseconds = seconds * NS_PER_SECOND
    if nanoseconds != nanoseconds.to_integral_value():
        raise argparse.ArgumentTypeError(
            "Measurement start time must be representable as whole nanoseconds."
        )

    return int(nanoseconds)


def update_measure_start_ns(lines, measure_start_ns: int) -> None:
    if not lines:
        return

    fields = lines[0].split()
    if len(fields) != 6 or fields[0] != "H":
        raise ValueError(
            "Expected trace header format: "
            "H <n_objs> <t_end_ns> <ix_pgs> <ml_pgs> <meas_start_ns>"
        )

    fields[5] = str(measure_start_ns)
    lines[0] = " ".join(fields) + "\n"


def keep_first_percentage(
    input_path: Path,
    output_path: Path,
    fraction: float,
    measure_start_ns: int,
) -> None:
    with input_path.open("r", encoding="utf-8", errors="replace") as input_file:
        lines = input_file.readlines()

    # Round upward so a nonempty file keeps at least one line for a positive percent.
    lines_to_keep = math.ceil(len(lines) * fraction)
    kept_lines = lines[:lines_to_keep]
    update_measure_start_ns(kept_lines, measure_start_ns)

    with output_path.open("w", encoding="utf-8") as output_file:
        output_file.writelines(kept_lines)

    print(f"Input file:    {input_path}")
    print(f"Total lines:   {len(lines)}")
    print(f"Percentage:    {fraction * 100:g}%")
    print(f"Lines kept:    {lines_to_keep}")
    print(f"Measure start: {measure_start_ns} ns")
    print(f"Output file:   {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Keep the first specified percentage of a trace log."
    )
    parser.add_argument(
        "input",
        type=Path,
        help="Input .txt log file",
    )
    parser.add_argument(
        "percentage",
        type=parse_percentage,
        help="Percentage to keep, such as 10%%, 10, or 0.1",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output filename",
    )
    parser.add_argument(
        "--meas-start",
        "--measure-start",
        dest="measure_start_ns",
        type=parse_measure_start_seconds,
        default=10_000_000_000,
        metavar="SECONDS",
        help=(
            "Measurement start time in seconds; written to the output trace "
            "header as nanoseconds (default: 10)"
        ),
    )

    args = parser.parse_args()

    if not args.input.is_file():
        parser.error(f"Input file does not exist: {args.input}")

    output_path = args.output
    if output_path is None:
        percent_label = f"{args.percentage * 100:g}".replace(".", "_")
        output_path = args.input.with_name(
            f"{args.input.stem}_{percent_label}"
            f"{args.input.suffix}"
        )

    try:
        keep_first_percentage(
            args.input,
            output_path,
            args.percentage,
            args.measure_start_ns,
        )
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
