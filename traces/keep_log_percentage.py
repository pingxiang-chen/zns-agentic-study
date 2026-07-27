#!/usr/bin/env python3

import argparse
import math
from pathlib import Path


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


def keep_first_percentage(
    input_path: Path,
    output_path: Path,
    fraction: float,
) -> None:
    with input_path.open("r", encoding="utf-8", errors="replace") as input_file:
        lines = input_file.readlines()

    # Round upward so a nonempty file keeps at least one line for a positive percent.
    lines_to_keep = math.ceil(len(lines) * fraction)

    with output_path.open("w", encoding="utf-8") as output_file:
        output_file.writelines(lines[:lines_to_keep])

    print(f"Input file:    {input_path}")
    print(f"Total lines:   {len(lines)}")
    print(f"Percentage:    {fraction * 100:g}%")
    print(f"Lines kept:    {lines_to_keep}")
    print(f"Output file:   {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Keep the first specified percentage of a text log."
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

    keep_first_percentage(args.input, output_path, args.percentage)


if __name__ == "__main__":
    main()