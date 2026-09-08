#!/usr/bin/env python3
"""Plot each available latency metric from NVMe *_summary.txt files separately.

Usage:
    python3 scripts/plot_nvme_summary_comparison.py results/nvme_occ90

Requires matplotlib. Writes PNG, PDF, and CSV files for each metric in
<results_dir>/plots by default. Missing metrics are skipped per configuration.
"""

import argparse
import csv
import math
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, MaxNLocator


METRICS = (
    "kv_write_cmd",
    "kv_write_op",
    "memlog_write_cmd",
    "memlog_read_cmd",
    "index_write_cmd",
    "index_read_cmd",
    "resume_burst",
    "turn_storage_stall",
)
STATISTICS = (
    ("mean", "Mean"),
    ("p50", "P50"),
    ("p90", "P90"),
    ("p99", "P99"),
    ("p999", "P99.9"),
    ("max", "Maximum"),
)
CONFIG_PATTERN = re.compile(r"(?:^|_)(conv|zns_k[1-9]\d*)_summary\.txt$")


def config_order(config):
    return (0, 0) if config == "conv" else (1, int(config.removeprefix("zns_k")))


def read_summaries(results_dir):
    results = {}
    for path in sorted(results_dir.glob("*_summary.txt")):
        match = CONFIG_PATTERN.search(path.name)
        if not match:
            continue
        config = match.group(1)
        if config in results:
            raise ValueError(f"Multiple summaries for {config} in {results_dir}")
        lines = path.read_text().splitlines()
        if "RUN_STATUS complete" not in lines:
            raise ValueError(f"{path}: run is not marked complete")
        metrics = {}
        for line in lines:
            parts = line.split()
            if len(parts) < 2 or parts[0] != "LAT" or parts[1] not in METRICS:
                continue
            metric = parts[1]
            if metric in metrics:
                raise ValueError(f"{path}: duplicate LAT {metric}")
            fields = dict(token.split("=", 1) for token in parts[2:] if "=" in token)
            if "n" not in fields:
                raise ValueError(f"{path}: {metric} missing n")
            count = int(fields["n"])
            if count == 0:
                print(f"Skipping {config}/{metric}: no observations")
                continue
            if count < 0:
                raise ValueError(f"{path}: {metric} has a negative observation count")
            required = {f"{stat}_us" for stat, _ in STATISTICS}
            missing = required - fields.keys()
            if missing:
                print(f"Skipping {config}/{metric}: missing {', '.join(sorted(missing))}")
                continue
            values = {
                f"{stat}_ms": float(fields[f"{stat}_us"]) / 1000
                for stat, _ in STATISTICS
            }
            if any(not math.isfinite(value) or value < 0 for value in values.values()):
                raise ValueError(f"{path}: {metric} has invalid latency values")
            metrics[metric] = {"n": count, **values}
        results[config] = {"source_file": path.name, "metrics": metrics}
    if not results:
        raise ValueError(f"{results_dir}: no conv or zns_k* summary files found")
    return dict(sorted(results.items(), key=lambda item: config_order(item[0])))


def write_metric_csv(results, metric, path):
    fields = ["configuration", "metric", "n"]
    fields += [f"{stat}_ms" for stat, _ in STATISTICS]
    fields += ["source_file"]
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for config, result in results.items():
            values = result["metrics"][metric]
            writer.writerow({
                "configuration": config,
                "metric": metric,
                "n": values["n"],
                **{f"{stat}_ms": format(values[f"{stat}_ms"], ".12g")
                   for stat, _ in STATISTICS},
                "source_file": result["source_file"],
            })


def format_latency(value, position=None):
    if 0 < abs(value) < 0.001:
        return f"{value:.2g}"
    return f"{value:,.3f}".rstrip("0").rstrip(".")


def plot_metric(results, metric, results_dir, output_dir, colors):
    configs = list(results)
    fig, axes = plt.subplots(2, 3, figsize=(17, 9.4))
    fig.subplots_adjust(left=0.065, right=0.985, top=0.84, bottom=0.14,
                        wspace=0.25, hspace=0.49)
    for ax, (stat, label) in zip(axes.flat, STATISTICS):
        values = [results[config]["metrics"][metric][f"{stat}_ms"]
                  for config in configs]
        bars = ax.bar(range(len(configs)), values, width=0.65,
                      color=[colors[config] for config in configs])
        ax.bar_label(bars, labels=[format_latency(value) for value in values],
                     padding=4, fontsize=8)
        ax.set_title(label, loc="left", fontweight="bold", pad=10)
        ax.set_ylabel("Latency (ms)")
        ax.set_xticks(range(len(configs)), configs, rotation=25, ha="right")
        ax.tick_params(axis="x", labelsize=9)
        ax.set_ylim(0, max(max(values) * 1.18, 0.001))
        ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
        ax.yaxis.set_major_formatter(FuncFormatter(format_latency))
        ax.grid(axis="y", color="#e5e7eb", linewidth=0.8)
        ax.set_axisbelow(True)

    fig.suptitle(f"{results_dir.name} | {metric}", x=0.065, y=0.974,
                 ha="left", fontsize=20, fontweight="bold")
    fig.text(0.065, 0.925,
             "Summary latency comparison across available NVMe configurations",
             color="#4b5563", fontsize=11)
    counts = {result["metrics"][metric]["n"] for result in results.values()}
    sample_note = (f"n={next(iter(counts)):,} per configuration" if len(counts) == 1
                   else "Sample counts vary by configuration; see CSV")
    fig.text(0.065, 0.054, sample_note, color="#4b5563", fontsize=10)
    fig.text(0.065, 0.023,
             "Source: *_summary.txt   |   Microseconds converted to milliseconds   |   "
             "Lower is better   |   Independent y-axis ranges; labels rounded",
             color="#6b7280", fontsize=9)
    for suffix in ("png", "pdf"):
        path = output_dir / f"{metric}_comparison.{suffix}"
        fig.savefig(path, dpi=200, facecolor="white")
        print(path)
    plt.close(fig)


def generate_comparisons(results_dir, output_dir=None):
    results = read_summaries(results_dir)
    output_dir = output_dir or results_dir / "plots"
    zns_configs = [config for config in results if config != "conv"]
    colors = {"conv": "#c0392b"}
    for index, config in enumerate(zns_configs):
        colors[config] = plt.get_cmap("viridis")(
            0.15 + 0.65 * index / max(len(zns_configs) - 1, 1)
        )
    with plt.rc_context({
        "font.family": "DejaVu Sans",
        "font.size": 10,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "pdf.fonttype": 42,
    }):
        for metric in METRICS:
            available = {config: result for config, result in results.items()
                         if metric in result["metrics"]}
            if not available:
                print(f"Skipping {metric}: absent from all summaries")
                continue
            missing = [config for config in results if config not in available]
            if missing:
                print(f"{metric}: skipping unavailable configurations {', '.join(missing)}")
            output_dir.mkdir(parents=True, exist_ok=True)
            csv_path = output_dir / f"{metric}_comparison.csv"
            write_metric_csv(available, metric, csv_path)
            print(csv_path)
            plot_metric(available, metric, results_dir, output_dir, colors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir", type=Path)
    parser.add_argument("--output-dir", "--out-dir", type=Path,
                        help="Output directory (default: <results_dir>/plots)")
    args = parser.parse_args()
    try:
        generate_comparisons(args.results_dir, args.output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
