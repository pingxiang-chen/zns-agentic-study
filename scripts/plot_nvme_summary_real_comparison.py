#!/usr/bin/env python3
"""Compare latency statistics from two NVMe summary files.

Usage:
    python3 plot_nvme_summary.py SUMMARY_A SUMMARY_B

The script writes one CSV and one PNG/PDF comparison plot per latency metric,
plus a consolidated CSV containing all shared metrics.  By default, output is
placed in a ``plots`` directory next to SUMMARY_A.
"""

import argparse
import csv
import math
import re
from collections import OrderedDict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, MaxNLocator


STATISTICS = (
    ("mean", "Mean"),
    ("p50", "P50"),
    ("p90", "P90"),
    ("p99", "P99"),
    ("p999", "P99.9"),
    ("max", "Maximum"),
)


def default_label(path):
    """Build a concise configuration label from a summary filename."""
    label = re.sub(r"_summary$", "", path.stem)
    label = re.sub(r"^(?:out_|result_|results_|real_)+", "", label)
    return label or path.stem


def read_summary(path, label):
    """Read and validate one NVMe summary file."""
    lines = path.read_text(encoding="utf-8").splitlines()
    if not any(line.strip() == "RUN_STATUS complete" for line in lines):
        raise ValueError(f"{path}: run is not marked complete")

    metrics = OrderedDict()
    for line_number, line in enumerate(lines, start=1):
        parts = line.split()
        if len(parts) < 2 or parts[0] != "LAT":
            continue

        metric = parts[1]
        if metric in metrics:
            raise ValueError(f"{path}:{line_number}: duplicate LAT {metric}")

        fields = {}
        for token in parts[2:]:
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value

        if "n" not in fields:
            raise ValueError(f"{path}:{line_number}: {metric} is missing n")
        try:
            count = int(fields["n"])
        except ValueError as error:
            raise ValueError(
                f"{path}:{line_number}: {metric} has an invalid n"
            ) from error
        if count < 0:
            raise ValueError(
                f"{path}:{line_number}: {metric} has a negative observation count"
            )
        if count == 0:
            print(f"Skipping {label}/{metric}: no observations")
            continue

        required = [f"{stat}_us" for stat, _ in STATISTICS]
        missing = [field for field in required if field not in fields]
        if missing:
            print(f"Skipping {label}/{metric}: missing {', '.join(missing)}")
            continue

        try:
            values = {
                f"{stat}_ms": float(fields[f"{stat}_us"]) / 1000.0
                for stat, _ in STATISTICS
            }
        except ValueError as error:
            raise ValueError(
                f"{path}:{line_number}: {metric} has a non-numeric latency"
            ) from error
        if any(not math.isfinite(value) or value < 0 for value in values.values()):
            raise ValueError(
                f"{path}:{line_number}: {metric} has an invalid latency value"
            )

        metrics[metric] = {"n": count, **values}

    if not metrics:
        raise ValueError(f"{path}: no usable LAT records found")
    return {
        "label": label,
        "source_file": str(path.resolve()),
        "metrics": metrics,
    }


def shared_metrics(first, second):
    """Return common metrics, preserving their order in the first file."""
    common = [metric for metric in first["metrics"] if metric in second["metrics"]]
    only_first = [metric for metric in first["metrics"] if metric not in second["metrics"]]
    only_second = [metric for metric in second["metrics"] if metric not in first["metrics"]]
    if only_first:
        print(f"Only in {first['label']}; not plotted: {', '.join(only_first)}")
    if only_second:
        print(f"Only in {second['label']}; not plotted: {', '.join(only_second)}")
    if not common:
        raise ValueError("the two summaries have no latency metrics in common")
    return common


def write_comparison_csv(results, metrics, path):
    fields = ["configuration", "metric", "n"]
    fields.extend(f"{stat}_ms" for stat, _ in STATISTICS)
    fields.append("source_file")
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for metric in metrics:
            for result in results:
                values = result["metrics"][metric]
                writer.writerow(
                    {
                        "configuration": result["label"],
                        "metric": metric,
                        "n": values["n"],
                        **{
                            f"{stat}_ms": format(values[f"{stat}_ms"], ".12g")
                            for stat, _ in STATISTICS
                        },
                        "source_file": result["source_file"],
                    }
                )


def write_metric_csv(results, metric, path):
    """Write the two configurations for one latency metric."""
    write_comparison_csv(results, [metric], path)


def format_latency(value, position=None):
    del position
    if 0 < abs(value) < 0.001:
        return f"{value:.2g}"
    return f"{value:,.3f}".rstrip("0").rstrip(".")


def safe_filename(value):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._") or "metric"


def plot_metric(results, metric, output_dir, formats):
    labels = [result["label"] for result in results]
    colors = ("#c0392b", "#2878b5")
    fig, axes = plt.subplots(2, 3, figsize=(17, 9.4))
    fig.subplots_adjust(
        left=0.065, right=0.985, top=0.84, bottom=0.14,
        wspace=0.25, hspace=0.49,
    )

    for ax, (stat, title) in zip(axes.flat, STATISTICS):
        values = [result["metrics"][metric][f"{stat}_ms"] for result in results]
        bars = ax.bar(range(2), values, width=0.62, color=colors)
        ax.bar_label(
            bars,
            labels=[format_latency(value) for value in values],
            padding=4,
            fontsize=8,
        )
        ax.set_title(title, loc="left", fontweight="bold", pad=10)
        ax.set_ylabel("Latency (ms)")
        ax.set_xticks(range(2), labels, rotation=20, ha="right")
        ax.tick_params(axis="x", labelsize=9)
        ax.set_ylim(0, max(max(values) * 1.18, 0.001))
        ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
        ax.yaxis.set_major_formatter(FuncFormatter(format_latency))
        ax.grid(axis="y", color="#e5e7eb", linewidth=0.8)
        ax.set_axisbelow(True)

    fig.suptitle(metric, x=0.065, y=0.974, ha="left", fontsize=20,
                 fontweight="bold")
    fig.text(
        0.065,
        0.925,
        f"NVMe latency comparison: {labels[0]} vs {labels[1]}",
        color="#4b5563",
        fontsize=11,
    )
    counts = [result["metrics"][metric]["n"] for result in results]
    count_note = (
        f"n={counts[0]:,} per configuration"
        if counts[0] == counts[1]
        else f"Sample counts: {labels[0]}={counts[0]:,}; {labels[1]}={counts[1]:,}"
    )
    fig.text(0.065, 0.054, count_note, color="#4b5563", fontsize=10)
    fig.text(
        0.065,
        0.023,
        "Microseconds converted to milliseconds  |  Lower is better  |  "
        "Independent y-axis ranges; labels rounded",
        color="#6b7280",
        fontsize=9,
    )

    for suffix in formats:
        output_path = output_dir / f"{safe_filename(metric)}_comparison.{suffix}"
        fig.savefig(output_path, dpi=200, facecolor="white")
        print(output_path)
    plt.close(fig)


def generate_comparison(summary_paths, output_dir, labels, formats):
    if summary_paths[0].resolve() == summary_paths[1].resolve():
        raise ValueError("please provide two different summary files")
    for path in summary_paths:
        if not path.is_file():
            raise ValueError(f"{path}: file not found")

    chosen_labels = labels or [default_label(path) for path in summary_paths]
    if chosen_labels[0] == chosen_labels[1]:
        raise ValueError(
            "the two configuration labels are identical; use --labels LABEL_A LABEL_B"
        )

    results = [
        read_summary(path, label)
        for path, label in zip(summary_paths, chosen_labels)
    ]
    metrics = shared_metrics(*results)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "nvme_summary_comparison.csv"
    write_comparison_csv(results, metrics, csv_path)
    print(csv_path)

    with plt.rc_context(
        {
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "pdf.fonttype": 42,
        }
    ):
        for metric in metrics:
            metric_csv_path = (
                output_dir / f"{safe_filename(metric)}_comparison.csv"
            )
            write_metric_csv(results, metric, metric_csv_path)
            print(metric_csv_path)
            plot_metric(results, metric, output_dir, formats)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary_a", type=Path, help="first *_summary.txt file")
    parser.add_argument("summary_b", type=Path, help="second *_summary.txt file")
    parser.add_argument(
        "--output-dir",
        "--out-dir",
        type=Path,
        help="output directory (default: plots next to SUMMARY_A)",
    )
    parser.add_argument(
        "--labels",
        nargs=2,
        metavar=("LABEL_A", "LABEL_B"),
        help="configuration labels (default: inferred from filenames)",
    )
    parser.add_argument(
        "--formats",
        nargs="+",
        choices=("png", "pdf"),
        default=("png", "pdf"),
        help="plot formats to write (default: png pdf)",
    )
    args = parser.parse_args()
    output_dir = args.output_dir or args.summary_a.parent / "plots"

    try:
        generate_comparison(
            [args.summary_a, args.summary_b],
            output_dir,
            args.labels,
            args.formats,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
