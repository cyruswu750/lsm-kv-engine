#!/usr/bin/env python3
"""Summarizes scripts/run_benchmarks.sh output: median and range over the
repetitions of each configuration, as Markdown tables and SVG bar charts.

Usage: summarize_bench.py <results dir>

Only runs recorded in raw/<group>.jsonl (exit 0 and ok=true) are used;
raw/failures.jsonl is listed separately and never summarized.
"""
import json
import os
import statistics
import sys


def load(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def by_label(rows):
    out = {}
    for r in rows:
        out.setdefault(r["label"], []).append(r["result"])
    return out


def stat(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return statistics.median(values), min(values), max(values), len(values)


def fmt(s, scale=1.0, digits=1):
    if s is None:
        return "n/a"
    med, lo, hi, n = s
    f = "%%.%df" % digits
    return (f % (med / scale)) + " [" + (f % (lo / scale)) + ", " + \
        (f % (hi / scale)) + "]" + ("" if n == 3 else " (n=%d)" % n)


def per_get(result, prefix, field):
    stats = result[prefix + "stats"]
    return stats[field] / stats["gets"] if stats["gets"] else None


def svg_bars(path, title, unit, groups):
    """groups: list of (label, (median, lo, hi, n))."""
    width, height, left, bottom, top = 640, 320, 70, 60, 40
    vals = [g[1] for g in groups if g[1] is not None]
    peak = max(v[2] for v in vals) if vals else 1
    peak = peak if peak > 0 else 1
    bar_w = (width - left - 20) / max(1, len(groups))
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
             'font-family="sans-serif" font-size="12">' % (width, height),
             '<rect width="100%" height="100%" fill="white"/>',
             '<text x="%d" y="20" font-size="14">%s</text>' % (left, title),
             '<text x="10" y="%d" transform="rotate(-90 10,%d)">%s</text>'
             % (height // 2, height // 2, unit)]
    plot_h = height - bottom - top
    for i, (label, s) in enumerate(groups):
        x = left + i * bar_w + bar_w * 0.15
        w = bar_w * 0.7
        if s is None:
            continue
        med, lo, hi, _ = s
        h = plot_h * med / peak
        y = top + plot_h - h
        parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" '
                     'fill="#4a78a8"/>' % (x, y, w, h))
        cx = x + w / 2
        y_lo = top + plot_h - plot_h * lo / peak
        y_hi = top + plot_h - plot_h * hi / peak
        parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
                     'stroke="black"/>' % (cx, y_lo, cx, y_hi))
        parts.append('<text x="%.1f" y="%.1f" text-anchor="middle">%.3g</text>'
                     % (cx, y - 4, med))
        parts.append('<text x="%.1f" y="%d" text-anchor="middle">%s</text>'
                     % (cx, height - bottom + 16, label))
    parts.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="black"/>'
                 % (left, top + plot_h, width - 10, top + plot_h))
    parts.append('<text x="%d" y="%d">bars: median; lines: min to max of '
                 'the repetitions</text>' % (left, height - 10))
    parts.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(parts) + "\n")


def main():
    out_dir = sys.argv[1]
    raw = os.path.join(out_dir, "raw")
    lines = ["# Benchmark summary", "",
             "Median [min, max] over the successful repetitions of each "
             "configuration (n=3 unless shown). Latencies are per DB call, "
             "from a monotonic clock, in microseconds, and include the two "
             "clock reads around the call (the measured overhead is listed "
             "below). Percentiles use the nearest rank. ops/s = calls / sum "
             "of per-call times; wall ops/s also counts the harness's untimed "
             "work between calls. Warm page cache; request counts are engine "
             "requests, not device I/O.", ""]
    failures = load(os.path.join(raw, "failures.jsonl"))
    lines.append("Failed runs (excluded): %d" % len(failures))
    for f in failures:
        lines.append("- %s %s rep %s exit %s" % (f["group"], f["label"],
                                                 f["rep"], f["exit"]))
    lines.append("")

    # Groups 1 and 2: inserts with sync on and off.
    ins = by_label(load(os.path.join(raw, "insert_sync.jsonl")))
    if ins:
        lines += ["## Inserts: sync on versus the sync bypass (groups 1 and 2)",
                  "",
                  "| config | ops/s | wall ops/s | p50 us | p99 us | max us | "
                  "write amp | flushes | compactions | skipped fsyncs |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for label in ("sync_on", "sync_off"):
            rs = ins.get(label, [])
            if not rs:
                continue
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                label,
                fmt(stat([r["insert_ops_per_s"] for r in rs]), 1, 0),
                fmt(stat([r.get("insert_wall_ops_per_s") for r in rs]), 1, 0),
                fmt(stat([r["insert_p50_ns"] for r in rs]), 1e3),
                fmt(stat([r["insert_p99_ns"] for r in rs]), 1e3),
                fmt(stat([r["insert_max_ns"] for r in rs]), 1e3),
                fmt(stat([r["write_amplification"] for r in rs]), 1, 3),
                fmt(stat([r["write_stats"]["flushes"] for r in rs]), 1, 0),
                fmt(stat([r["write_stats"]["compactions"] for r in rs]), 1, 0),
                json.dumps(rs[0]["fsync_skipped"])))
        on = stat([r["insert_ops_per_s"] for r in ins.get("sync_on", [])])
        off = stat([r["insert_ops_per_s"] for r in ins.get("sync_off", [])])
        if on and off:
            lines += ["", "Throughput ratio (median off / median on): %.1fx. "
                      "Skipping sync is unsafe and only a reference point; "
                      "the difference is not an exact fsync time share." %
                      (off[0] / on[0])]
        lines.append("")
        svg_bars(os.path.join(out_dir, "insert_sync.svg"),
                 "Insert throughput, sync on vs bypass", "ops/s",
                 [(l, stat([r["insert_ops_per_s"] for r in ins.get(l, [])]))
                  for l in ("sync_on", "sync_off")])

    # Group 1 (reads) and group 3: Bloom queries used or bypassed.
    rb = by_label(load(os.path.join(raw, "read_bloom.jsonl")))
    if rb:
        lines += ["## Lookups: Bloom queries used versus bypassed (groups 1 "
                  "and 3)", "",
                  "Same prepared files, same lookup traces. Per-lookup "
                  "counters are per Get.", "",
                  "| config | kind | ops/s | p50 us | p99 us | candidates | "
                  "filter rejections | range rejections | data blocks | "
                  "bytes requested |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for label in ("bypass_0", "bypass_1"):
            rs = rb.get(label, [])
            for kind in ("existing", "missing"):
                p = kind + "_"
                lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | "
                             "%s |" % (
                    "filters used" if label == "bypass_0" else "bypassed",
                    kind,
                    fmt(stat([r[kind + "_ops_per_s"] for r in rs]), 1, 0),
                    fmt(stat([r[kind + "_p50_ns"] for r in rs]), 1e3, 2),
                    fmt(stat([r[kind + "_p99_ns"] for r in rs]), 1e3, 2),
                    fmt(stat([per_get(r, p, "candidate_ssts") for r in rs]),
                        1, 3),
                    fmt(stat([per_get(r, p, "filter_rejections") for r in rs]),
                        1, 3),
                    fmt(stat([per_get(r, p, "range_rejections") for r in rs]),
                        1, 3),
                    fmt(stat([per_get(r, p, "data_block_reads") for r in rs]),
                        1, 3),
                    fmt(stat([per_get(r, p, "data_block_bytes") for r in rs]),
                        1, 0)))
        on = stat([per_get(r, "missing_", "data_block_reads")
                   for r in rb.get("bypass_0", [])])
        off = stat([per_get(r, "missing_", "data_block_reads")
                    for r in rb.get("bypass_1", [])])
        prep = json.load(open(os.path.join(raw, "prepare.json")))
        lines += ["", "Prepared database: %d keys, %d live runs, %d live SSTs, "
                  "%d bytes." % (prep["ops"], prep["live_runs"],
                                 prep["live_ssts"], prep["db_bytes"])]
        if on and off and off[0]:
            lines.append("Missing-key data-block requests per lookup: %.3f "
                         "bypassed, %.3f with filters (%.1f%% fewer)." %
                         (off[0], on[0], 100 * (1 - on[0] / off[0])))
        rs = rb.get("bypass_0", [])
        if rs:
            lines.append("Open phase (filters used runs): %s ms, %s bytes "
                         "requested by pread." % (
                fmt(stat([r["open_ns"] for r in rs]), 1e6),
                fmt(stat([r["open_pread_bytes"] for r in rs]), 1, 0)))
        lines.append("")
        svg_bars(os.path.join(out_dir, "read_bloom_blocks.svg"),
                 "Missing-key data-block requests per lookup",
                 "requests / Get",
                 [("filters used", on), ("bypassed", off)])
        svg_bars(os.path.join(out_dir, "read_bloom_p50.svg"),
                 "Missing-key lookup p50", "us",
                 [("filters used",
                   stat([r["missing_p50_ns"] / 1e3 for r in rb.get("bypass_0", [])])),
                  ("bypassed",
                   stat([r["missing_p50_ns"] / 1e3 for r in rb.get("bypass_1", [])]))])

    # Group 4: compaction run thresholds.
    th = by_label(load(os.path.join(raw, "threshold.jsonl")))
    if th:
        lines += ["## Compaction run thresholds (group 4)", "",
                  "Same trace (overwrites, 10% deletes), same fresh initial "
                  "state, same final operation; no forced maintenance. "
                  "Database size counts every regular file in the directory "
                  "(including any leftover orphan or temporary file) after "
                  "the handle is closed.", "",
                  "| trigger | ops/s | wall ops/s | p50 us | p99 us | max us | "
                  "write amp | metadata / user byte | DB bytes | live runs | "
                  "live SSTs | flushes | compactions | SST candidates per Get |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        order = ("trigger_2", "trigger_4", "trigger_8", "trigger_0")
        for label in order:
            rs = th.get(label, [])
            if not rs:
                continue
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | "
                         "%s | %s | %s | %s |" % (
                label.replace("trigger_0", "disabled").replace("trigger_", ""),
                fmt(stat([r["insert_ops_per_s"] for r in rs]), 1, 0),
                fmt(stat([r.get("insert_wall_ops_per_s") for r in rs]), 1, 0),
                fmt(stat([r["insert_p50_ns"] for r in rs]), 1e3),
                fmt(stat([r["insert_p99_ns"] for r in rs]), 1e3),
                fmt(stat([r["insert_max_ns"] for r in rs]), 1e3),
                fmt(stat([r["write_amplification"] for r in rs]), 1, 3),
                fmt(stat([r["metadata_per_user_byte"] for r in rs]), 1, 4),
                fmt(stat([r["db_bytes"] for r in rs]), 1, 0),
                fmt(stat([r["live_runs"] for r in rs]), 1, 0),
                fmt(stat([r["live_ssts"] for r in rs]), 1, 0),
                fmt(stat([r["write_stats"]["flushes"] for r in rs]), 1, 0),
                fmt(stat([r["write_stats"]["compactions"] for r in rs]), 1, 0),
                fmt(stat([per_get(r, "verify_read_", "candidate_ssts")
                          for r in rs]), 1, 3)))
        lines.append("")
        names = {"trigger_2": "2", "trigger_4": "4", "trigger_8": "8",
                 "trigger_0": "off"}
        svg_bars(os.path.join(out_dir, "threshold_write_amp.svg"),
                 "Write amplification by run threshold", "written / user bytes",
                 [(names[l], stat([r["write_amplification"]
                                   for r in th.get(l, [])])) for l in order])
        svg_bars(os.path.join(out_dir, "threshold_p99.svg"),
                 "Insert p99 by run threshold (includes maintenance)", "us",
                 [(names[l], stat([r["insert_p99_ns"] / 1e3
                                   for r in th.get(l, [])])) for l in order])
        svg_bars(os.path.join(out_dir, "threshold_candidates.svg"),
                 "SST candidates per lookup after the trace", "SSTs / Get",
                 [(names[l], stat([per_get(r, "verify_read_", "candidate_ssts")
                                   for r in th.get(l, [])])) for l in order])

    overheads = [r["result"].get("timer_overhead_ns")
                 for g in ("insert_sync", "read_bloom", "threshold")
                 for r in load(os.path.join(raw, g + ".jsonl"))]
    o = stat([v for v in overheads if v is not None])
    if o:
        lines += ["Clock overhead per timed call (median of 10,001 back-to-back "
                  "steady_clock pairs, per run): %s ns [%s, %s] over %d runs. "
                  "It is included in every latency above and matters only "
                  "for sub-microsecond calls." %
                  (o[0], o[1], o[2], o[3]), ""]
    with open(os.path.join(out_dir, "summary.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
