#!/usr/bin/env python3
"""Markdown tables from a results tree.

  report.py RESULTS

- Reads every row.csv (one per run) and result.json (one per comparison, from compare.py) under
  RESULTS.
- Writes RESULTS/report.md: the comparisons and their verdicts against the predictions, then
  per-variant medians over the valid runs. Also writes RESULTS/runs.csv with every run.
- Percentiles are counter ticks (41.67 ns). Below 3 ticks a reading k only says the true value
  lies in (k - 1, k + 1) ticks, so those are printed as intervals, not points.
"""
import csv
import json
import os
import statistics
import sys

NS_PER_TICK = 125.0 / 3.0  # the M1 counter runs at 24 MHz
POINT_READING_MIN_TICKS = 3  # below this a percentile is printed as an interval
NS_BEFORE_MICROSECONDS = 1e4  # at or above this, print microseconds instead of nanoseconds
P_CORE_PLACEMENT = "pp"  # old runs: writer and fast reader both on P-cores


def find_files(root, name):
    """Every file called name under root, in sorted directory order."""
    for directory, _, files in sorted(os.walk(root)):
        if name in files:
            yield os.path.join(directory, name)


def to_float_or_none(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def column_median(rows, column):
    """Median of the column over rows that hold a number there, or None if none do."""
    values = [value for value in (to_float_or_none(row.get(column)) for row in rows)
              if value is not None]
    return statistics.median(values) if values else None


def format_ticks(ticks):
    """A tick count as ns or us, or as an interval below POINT_READING_MIN_TICKS."""
    if ticks is None:
        return ""
    if ticks < POINT_READING_MIN_TICKS:
        whole_ticks = int(round(ticks))
        return "%d t, in (%.0f, %.0f) ns" % (whole_ticks, max(0, whole_ticks - 1) * NS_PER_TICK,
                                            (whole_ticks + 1) * NS_PER_TICK)
    nanoseconds = ticks * NS_PER_TICK
    if nanoseconds < NS_BEFORE_MICROSECONDS:
        return "%.0f ns" % nanoseconds
    return "%.1f us" % (nanoseconds / 1e3)


def comparison_table(results):
    """One row per comparison; one with too few usable pairs shows its verdict and why."""
    out = ["| id | A -> B | metric | A median | B - A median | range | agree | sign p | left out "
           "| predicted | verdict |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    for result in sorted(results, key=lambda result: result["id"]):
        left_out = result.get("invalid_pairs", 0) + result.get("stalled_pairs", 0)
        if "median" not in result:
            out.append("| %s | %s -> %s | %s | | | | | | %d | | %s (%s) |" % (
                result["id"], result["a"], result["b"], result["metric"], left_out,
                result["verdict"], result.get("why", "")))
            continue
        hit = "hit" if result["prediction_hit"] else "miss"
        out.append("| %s | %s -> %s | %s | %.4g | %+.3g | [%.3g, %.3g] | %d/%d | %.3f | %d "
                   "| [%.3g, %.3g] %s | %s |" % (
                       result["id"], result["a"], result["b"], result["metric"],
                       result["a_median"], result["median"], result["lo"], result["hi"],
                       result["agree"], result["n"], result["sign_p"], left_out,
                       result["predicted"][0], result["predicted"][1], hit, result["verdict"]))
    return out


def scenario_table(valid_rows, scenario, keys, columns):
    """One table for a scenario's valid runs: a row per distinct value of keys, the median of
    each of columns. Empty when the scenario has no valid runs."""
    scenario_rows = [row for row in valid_rows if row.get("scenario") == scenario]
    if not scenario_rows:
        return []
    out = ["", "| %s | runs | %s |" % (" | ".join(keys), " | ".join(columns)),
           "|---" * (len(keys) + len(columns) + 1) + "|"]
    for row_key in sorted({tuple(row.get(key, "") for key in keys) for row in scenario_rows}):
        matching_rows = [row for row in scenario_rows
                         if tuple(row.get(key, "") for key in keys) == row_key]
        medians = " | ".join("%.4g" % (column_median(matching_rows, column) or 0)
                             for column in columns)
        out.append("| %s | %d | %s |" % (" | ".join(row_key), len(matching_rows), medians))
    return out


def paced_latency_table(valid_rows):
    """Hop and e2e latency of the paced runs with a fast reader, per (variant, slow, relay)."""
    cells = {}
    for row in valid_rows:
        # Runs from before 2026-10-04 carry a placement column, and some put the fast reader on
        # an E-core: only their "pp" runs are kept. Newer runs have no such column (the fast
        # reader is always on a P-core), so a missing placement counts as "pp".
        if (row.get("scenario") == "paced" and row.get("fast") == "1"
                and row.get("placement", P_CORE_PLACEMENT) == P_CORE_PLACEMENT):
            key = (row["variant"], row.get("slow", "off"), row.get("relay", "0"))
            cells.setdefault(key, []).append(row)
    out = ["| variant | slow | relay | runs | metric | body mean | mean | p50 | p99 | p99.9 "
           "| max |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    for (variant, slow, relay), cell_rows in sorted(cells.items()):
        for metric in ("hop", "e2e"):
            body_mean = column_median(cell_rows, metric + "_mean_body_ns")
            mean = column_median(cell_rows, metric + "_mean_ns")
            percentiles = [format_ticks(column_median(cell_rows, "%s_%s" % (metric, percentile)))
                           for percentile in ("p50", "p99", "p999", "max")]
            out.append("| %s | %s | %s | %d | %s | %s | %s | %s |" % (
                variant, slow, "on" if relay == "1" else "off", len(cell_rows), metric,
                "" if body_mean is None else "%.1f ns" % body_mean,
                "" if mean is None else "%.1f ns" % mean,
                " | ".join(percentiles)))
    return out


def per_variant_tables(rows):
    """The per-variant section: paced latency, then the W-sat, dispatch and feed tables."""
    valid_rows = [row for row in rows if row.get("valid") == "1"]
    out = paced_latency_table(valid_rows)
    out += scenario_table(valid_rows, "wsat", ("variant", "fast"),
                          ["wsat_ns", "wsat_instr", "lost_fast"])
    out += scenario_table(valid_rows, "dispatch", ("dispatch",), ["dispatch_ns", "dispatch_instr"])
    out += scenario_table(valid_rows, "feed", ("variant",),
                          ["ns_per_event", "instr_per_event", "cycles_per_event"])
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        sys.exit(__doc__)
    root = argv[0]
    rows = []
    for path in find_files(root, "row.csv"):
        with open(path) as row_file:
            rows += list(csv.DictReader(row_file))
    results = []
    for path in find_files(root, "result.json"):
        with open(path) as result_file:
            results.append(json.load(result_file))
    valid_count = sum(row.get("valid") == "1" for row in rows)
    report_lines = ["# mdbus benchmark report", "",
                    "Runs: %d (%d valid). Comparisons: %d." % (len(rows), valid_count,
                                                                len(results)),
                    "", "## Comparisons (B - A over counterbalanced pairs)", ""]
    report_lines += comparison_table(results)
    report_lines += ["", "## Latency per variant (medians over valid runs)", ""]
    report_lines += per_variant_tables(rows)
    with open(os.path.join(root, "report.md"), "w") as report_file:
        report_file.write("\n".join(report_lines) + "\n")
    # Union of every row's columns, in first-seen order: older runs lack some columns.
    columns = list(dict.fromkeys(column for row in rows for column in row))
    with open(os.path.join(root, "runs.csv"), "w", newline="") as runs_file:
        csv_writer = csv.DictWriter(runs_file, fieldnames=columns)
        csv_writer.writeheader()
        csv_writer.writerows(rows)
    print("report: %s" % os.path.join(root, "report.md"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
