#!/usr/bin/env python3
"""Plot trace-session timelines or separate summary latency comparisons.

Summary comparisons (one PNG, PDF, and CSV per available metric):
    python3 scripts/plot_first_sessions_absolute_writes.py results/nvme_occ90

Trace timelines:
    python3 scripts/plot_first_sessions_absolute_writes.py --trace traces/tr_occ90.txt
"""

import argparse
import csv
import gzip
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


PG = 4096
NS = 1_000_000_000
MIB = 1 << 20
GIB = 1 << 30

COLORS = {
    "WKV": "#2471a3",
    "WML": "#2ca25f",
    "WIX": "#d95f0e",
}

LABELS = {
    "WKV": "KV blob append",
    "WML": "memory-log append",
    "WIX": "vector-index update",
}

MARKERS = ["o", "s", "^", "D", "P", "X", "v", "<", ">"]


@dataclass(frozen=True)
class Op:
    sid: int
    t_ns: int
    kind: str
    obj: int
    off_pgs: int
    len_pgs: int
    turn_start: int


def open_maybe_gzip(path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt")
    return path.open("rt")


def resolve_trace(path):
    if path.exists():
        return path
    gz = Path(str(path) + ".gz")
    if gz.exists():
        return gz
    raise FileNotFoundError(f"trace not found: {path}")


def parse_trace(path):
    path = resolve_trace(path)
    starts = {}
    sessions = defaultdict(list)
    sid = None
    cur_ns = 0

    with open_maybe_gzip(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "A":
                sid = int(parts[1])
                cur_ns = int(parts[2])
                starts[sid] = cur_ns
            elif parts[0] == "O":
                cur_ns += int(parts[1])
                sessions[sid].append(
                    Op(
                        sid=sid,
                        t_ns=cur_ns,
                        kind=parts[2],
                        obj=int(parts[3]),
                        off_pgs=int(parts[4]),
                        len_pgs=int(parts[5]),
                        turn_start=int(parts[6]),
                    )
                )
    return path, starts, sessions


def first_sessions(starts, count):
    return sorted(sid for sid in starts if sid != 0)[:count]


def build_rows(starts, sessions, selected_sids):
    rows = []
    ends = {}
    ended = {}

    for sid in selected_sids:
        ops = sessions.get(sid, [])
        start = starts[sid]
        last = start
        saw_del = False
        for op in ops:
            last = max(last, op.t_ns)
            if op.kind == "DEL":
                saw_del = True
            if op.kind not in COLORS or op.len_pgs <= 0:
                continue
            rows.append(
                {
                    "sid": sid,
                    "time_s": op.t_ns / NS,
                    "relative_time_s": (op.t_ns - start) / NS,
                    "kind": op.kind,
                    "label": LABELS[op.kind],
                    "obj": op.obj,
                    "off_mib": op.off_pgs * PG / MIB,
                    "len_kib": op.len_pgs * PG / 1024,
                    "len_mib": op.len_pgs * PG / MIB,
                    "turn_start": op.turn_start,
                }
            )
        ends[sid] = last
        ended[sid] = saw_del
    rows.sort(key=lambda r: (r["time_s"], r["sid"], r["kind"]))
    return rows, ends, ended


def scaled_marker_sizes(rows):
    if not rows:
        return []
    max_len = max(r["len_mib"] for r in rows)
    denom = math.log1p(max_len) if max_len > 0 else 1.0
    return [13 + 46 * math.log1p(r["len_mib"]) / denom for r in rows]


def plot(rows, starts, ends, ended, selected_sids, out_png):
    fig, axes = plt.subplots(
        3,
        1,
        figsize=(11.5, 8.8),
        sharex=True,
        gridspec_kw={"height_ratios": [1.1, 1.1, 0.85]},
    )

    x0 = min(starts[sid] for sid in selected_sids) / NS
    x1 = max(ends[sid] for sid in selected_sids) / NS
    pad = max(5.0, 0.02 * (x1 - x0))

    for i, sid in enumerate(selected_sids):
        marker = MARKERS[i % len(MARKERS)]
        for kind in ("WKV", "WML", "WIX"):
            sub = [r for r in rows if r["sid"] == sid and r["kind"] == kind]
            if not sub:
                continue
            sizes = scaled_marker_sizes(sub)
            axes[0].scatter(
                [r["time_s"] for r in sub],
                [r["len_kib"] for r in sub],
                s=sizes,
                color=COLORS[kind],
                marker=marker,
                alpha=0.66,
                edgecolors="none",
            )
            axes[1].scatter(
                [r["time_s"] for r in sub],
                [r["off_mib"] for r in sub],
                s=sizes,
                color=COLORS[kind],
                marker=marker,
                alpha=0.60,
                edgecolors="none",
            )

    write_handles = [
        Line2D(
            [0],
            [0],
            marker="o",
            linestyle="None",
            markerfacecolor=COLORS[kind],
            markeredgecolor="none",
            markersize=8,
            label=LABELS[kind],
        )
        for kind in ("WKV", "WML", "WIX")
    ]
    session_handles = [
        Line2D(
            [0],
            [0],
            marker=MARKERS[i % len(MARKERS)],
            linestyle="None",
            markerfacecolor="#4a4a4a",
            markeredgecolor="none",
            markersize=8,
            label=f"sid {sid}",
        )
        for i, sid in enumerate(selected_sids)
    ]
    event_handles = [
        Line2D(
            [0],
            [0],
            marker="o",
            linestyle="None",
            markerfacecolor="#111111",
            markeredgecolor="#111111",
            markersize=8,
            label="arrival (filled session marker)",
        ),
        Line2D(
            [0],
            [0],
            marker="o",
            linestyle="None",
            markerfacecolor="white",
            markeredgecolor="#111111",
            markersize=8,
            label="DEL/end (open session marker)",
        ),
    ]

    axes[0].set_yscale("log")
    axes[0].set_ylabel("write size (KiB, log)")
    if len(selected_sids) == 1:
        title = f"Session {selected_sids[0]} on the absolute trace-time scale"
    else:
        title = f"First {len(selected_sids)} sessions on the same absolute trace-time scale"
    axes[0].set_title(title)
    axes[0].grid(alpha=0.3, which="both")
    leg_write = axes[0].legend(handles=write_handles, title="write type", fontsize=8, title_fontsize=8, ncol=3, loc="upper right")
    axes[0].add_artist(leg_write)
    axes[0].legend(
        handles=session_handles,
        title="session marker",
        fontsize=8,
        title_fontsize=8,
        ncol=min(len(session_handles), 5),
        loc="lower right",
    )

    axes[1].set_ylabel("logical offset (MiB)")
    axes[1].grid(alpha=0.3)

    ax = axes[2]
    sid_to_y = {sid: len(selected_sids) - 1 - i for i, sid in enumerate(selected_sids)}
    for i, sid in enumerate(selected_sids):
        y = sid_to_y[sid]
        marker = MARKERS[i % len(MARKERS)]
        start_s = starts[sid] / NS
        end_s = ends[sid] / NS
        ax.hlines(y, start_s, end_s, color="#777777", lw=2.0, alpha=0.72)
        ax.scatter(
            [start_s],
            [y],
            marker=marker,
            s=74,
            facecolors="#111111",
            edgecolors="#111111",
            linewidths=0.8,
            zorder=4,
        )
        ax.scatter(
            [end_s],
            [y],
            marker=marker,
            s=82,
            facecolors="white" if ended[sid] else "#eeeeee",
            edgecolors="#111111" if ended[sid] else "#777777",
            linewidths=1.2,
            zorder=4,
        )

    for i, sid in enumerate(selected_sids):
        marker = MARKERS[i % len(MARKERS)]
        for kind in ("WKV", "WML", "WIX"):
            sub = [r for r in rows if r["sid"] == sid and r["kind"] == kind]
            if not sub:
                continue
            ax.scatter(
                [r["time_s"] for r in sub],
                [sid_to_y[r["sid"]] for r in sub],
                s=scaled_marker_sizes(sub),
                color=COLORS[kind],
                marker=marker,
                alpha=0.48 if kind == "WKV" else 0.34,
                edgecolors="none",
            )

    ax.set_yticks([sid_to_y[sid] for sid in selected_sids])
    ax.set_yticklabels([f"sid {sid}" for sid in selected_sids])
    ax.set_ylabel("session")
    ax.set_xlabel("absolute trace time (s)")
    ax.set_xlim(x0 - pad, x1 + pad)
    ax.grid(alpha=0.25, axis="x")
    leg_event = ax.legend(handles=event_handles, title="lifetime marker", fontsize=8, title_fontsize=8, ncol=2, loc="upper left")
    ax.add_artist(leg_event)
    leg_write = ax.legend(handles=write_handles, title="write type", fontsize=8, title_fontsize=8, ncol=3, loc="upper right")
    ax.add_artist(leg_write)
    ax.legend(
        handles=session_handles,
        title="session marker",
        fontsize=8,
        title_fontsize=8,
        ncol=min(len(session_handles), 5),
        loc="lower right",
    )

    for axis in axes:
        for sid in selected_sids:
            axis.axvline(starts[sid] / NS, color="#444444", lw=0.6, alpha=0.18, linestyle="--")

    caption = "Markers keep real arrival time; no per-session time alignment is applied. Marker shape identifies session, color identifies write type."
    fig.text(0.01, 0.01, caption, fontsize=9)
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(out_png, dpi=300)
    plt.close(fig)


def write_csv(rows, starts, ends, ended, selected_sids, out_csv):
    fields = [
        "sid",
        "arrival_s",
        "end_s",
        "lifetime_s",
        "has_del",
        "time_s",
        "relative_time_s",
        "kind",
        "label",
        "obj",
        "off_mib",
        "len_kib",
        "len_mib",
        "turn_start",
    ]
    with out_csv.open("w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        for row in rows:
            sid = row["sid"]
            out = dict(row)
            out["arrival_s"] = starts[sid] / NS
            out["end_s"] = ends[sid] / NS
            out["lifetime_s"] = (ends[sid] - starts[sid]) / NS
            out["has_del"] = int(ended[sid])
            for key in ("arrival_s", "end_s", "lifetime_s", "time_s", "relative_time_s", "off_mib", "len_kib", "len_mib"):
                out[key] = f"{out[key]:.6f}"
            wr.writerow({k: out[k] for k in fields})


def write_md(rows, starts, ends, ended, selected_sids, out_md):
    lines = [
        "# First sessions absolute-time write timeline",
        "",
        "| Session | Arrival s | End s | Lifetime s | DEL | WKV ops | WML ops | WIX ops | Total MiB |",
        "|---:|---:|---:|---:|:---:|---:|---:|---:|---:|",
    ]
    for sid in selected_sids:
        sub = [r for r in rows if r["sid"] == sid]
        counts = {kind: sum(1 for r in sub if r["kind"] == kind) for kind in ("WKV", "WML", "WIX")}
        total_mib = sum(r["len_mib"] for r in sub)
        lines.append(
            f"| {sid} | {starts[sid] / NS:.6f} | {ends[sid] / NS:.6f} | "
            f"{(ends[sid] - starts[sid]) / NS:.3f} | {'yes' if ended[sid] else 'no'} | "
            f"{counts['WKV']} | {counts['WML']} | {counts['WIX']} | {total_mib:.2f} |"
        )
    lines.append("")
    lines.append(
        "Interpretation: this view preserves the trace's real arrival schedule. Each plotted session starts at its own arrival point and continues to its DEL/end."
    )
    out_md.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("results_dir", type=Path, nargs="?",
                    help="Directory of conv/zns_k* *_summary.txt files to compare")
    ap.add_argument("--trace", type=Path,
                    help="Trace for session timelines (default: ../traces/tr_occ90.txt)")
    ap.add_argument("--count", type=int, default=3, help="Number of trace sessions")
    ap.add_argument("--out-dir", "--output-dir", type=Path,
                    help="Output directory (summary: <results_dir>/plots; trace: ../figs)")
    ap.add_argument("--prefix", default="3", help="Filename prefix for trace output")
    args = ap.parse_args()

    if args.results_dir is not None:
        if args.trace is not None:
            ap.error("results_dir and --trace cannot be used together")
        from plot_nvme_summary_comparison import generate_comparisons

        try:
            generate_comparisons(args.results_dir, args.out_dir)
        except (OSError, ValueError) as error:
            ap.error(str(error))
        return

    args.trace = args.trace or Path("../traces/tr_occ90.txt")
    args.out_dir = args.out_dir or Path("../figs")
    trace_path, starts, sessions = parse_trace(args.trace)
    selected_sids = first_sessions(starts, args.count)
    if not selected_sids:
        raise ValueError("no non-system sessions found")

    rows, ends, ended = build_rows(starts, sessions, selected_sids)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    
    out_png = args.out_dir / f"{args.prefix}.png"
    out_csv = args.out_dir / f"{args.prefix}.csv"
    out_md = args.out_dir / f"{args.prefix}.md"

    print(out_png)

    plot(rows, starts, ends, ended, selected_sids, out_png)
    write_csv(rows, starts, ends, ended, selected_sids, out_csv)
    write_md(rows, starts, ends, ended, selected_sids, out_md)

    print(f"trace {trace_path}")
    print("sessions " + ",".join(str(sid) for sid in selected_sids))
    print(f"wrote {out_png}")
    print(f"wrote {out_csv}")
    print(f"wrote {out_md}")


if __name__ == "__main__":
    main()
