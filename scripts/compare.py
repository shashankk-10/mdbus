#!/usr/bin/env python3
"""Counterbalanced paired comparisons of two benchmark binaries, as listed in bench/decisions.yaml.

  compare.py Copy                       one comparison: 6 pairs of 3 s runs, into results/raw/Copy
  compare.py --all                      every comparison, A/A noise floors first, then the report
  compare.py --all --only Copy,Pad-pp   a subset
  compare.py --all --dry-run            the plan and its time estimate

- Pairs alternate AB, BA, AB, ..., so drift and warm-up land on both sides alike.
- The run, not the sample, is the unit: samples within a run are autocorrelated (one OS stall is
  thousands of slow messages in a row). Per pair, d = B - A.
- Reported: the median of d, its range over the pairs, how many pairs agree in sign, the exact
  two-sided sign test, and whether the prediction written beforehand holds. With 6 pairs the
  smallest possible p is 0.031, when all 6 agree.
- A run the gates rejected (it measured a busy machine) is re-run up to twice; if it still
  fails, its pair is left out and counted.
- For a body-mean metric, a pair whose two runs trimmed different shares of messages (one side
  hit an OS stall) is also left out and counted: the two means describe different messages.
- Verdict: "B better" or "B worse" when the sign test passes and the median clears both the
  practical threshold and the A/A noise floor; "no difference" when the whole range lies inside
  that band; otherwise "unresolved".
- Finished runs are kept, so an interrupted series resumes.
"""
import argparse
import csv
import json
import math
import os
import statistics
import subprocess
import sys
import time

import yaml

import report

HERE = os.path.dirname(os.path.abspath(__file__))
DECISIONS = os.path.join(HERE, "..", "bench", "decisions.yaml")
SIGNIFICANCE_LEVEL = 0.05  # two-sided sign test p at or below this counts as a real difference
MIN_USABLE_PAIRS = 3  # fewer pairs than this and no verdict is attempted
MAX_TRIMMED_SHARE_DIFF = 0.01  # largest difference in trimmed share for a body-mean pair to count
MAX_RERUNS = 2  # re-runs of a gate-rejected run before its pair is given up
DEFAULT_PAIRS = 6
DEFAULT_DURATION_MS = 3000
DEFAULT_COOLDOWN_SECONDS = 30.0  # between comparisons, so heat does not carry over
# The column holding the share of samples trimmed from each body-mean metric.
TRIM_COLUMN = {"hop_mean_body_ns": "hop_trim_frac", "e2e_mean_body_ns": "e2e_trim_frac"}
FEED_METRIC = "ns_per_event"  # only feed_bench reports it
FEED_RUN_SECONDS = 6.0  # one feed run: generating the stream, the warm-up and the replay
BUS_RUN_OVERHEAD_SECONDS = 1.3  # one bus run beyond its window: warm-up, spawning and reaping


def load_decisions(path=DECISIONS):
    with open(path) as decisions_file:
        return yaml.safe_load(decisions_file)["decisions"]


def binary(bin_dir, variant):
    return os.path.join(bin_dir, "mdbus_bench_" + variant)


def read_row(run_dir):
    """The run's row.csv as a dict, or None if the run left no row."""
    try:
        with open(os.path.join(run_dir, "row.csv")) as row_file:
            return next(csv.DictReader(row_file))
    except (OSError, StopIteration):
        return None


def number_or_none(row, column):
    """The column as a finite float, or None if it is missing, empty or not finite."""
    try:
        value = float(row[column])
    except (KeyError, TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def valid(row):
    return row is not None and row.get("valid") == "1"


def run_dirs(out, pairs):
    """[(a_dir, b_dir, order)] for pair i: AB on even i, BA on odd i."""
    return [(os.path.join(out, "p%02d_A" % i), os.path.join(out, "p%02d_B" % i),
             "AB" if i % 2 == 0 else "BA")
            for i in range(pairs)]


def run_series(decision, bin_dir, out, pairs, duration_ms, retries=MAX_RERUNS):
    """Runs each pair's missing or invalid runs. Returns the re-runs each side needed.

    - Both runs of pair i get --seed i + 1, so A and B see the same workload.
    """
    reruns = {"A": 0, "B": 0}
    os.makedirs(out, exist_ok=True)  # a bench creates its own run directory, not the parents
    for i, (a_dir, b_dir, order) in enumerate(run_dirs(out, pairs)):
        for side in order:
            variant, run_dir = (decision["a"], a_dir) if side == "A" else (decision["b"], b_dir)
            side_args = decision.get("args", []) + decision.get("args_" + side.lower(), [])
            cmd = [binary(bin_dir, variant)] + [str(arg) for arg in side_args]
            cmd += ["--seed", str(i + 1), "--duration-ms", str(duration_ms), "--out", run_dir]
            for attempt in range(retries + 1):
                if valid(read_row(run_dir)):
                    break
                if attempt > 0:
                    reruns[side] += 1
                return_code = subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode
                row = read_row(run_dir)
                why = ("" if valid(row)
                       else " gates failed: " + (row or {}).get("reject_reason", "no row"))
                print("  pair %d %s %-10s rc=%d attempt %d%s"
                      % (i, side, variant, return_code, attempt + 1, why), flush=True)
    return reruns


def median_or_none(values):
    if not values:
        return None  # statistics.median raises on an empty list
    return statistics.median(values)


def sign_test(pair_diffs):
    """Exact two-sided sign test p value, ties dropped.

    sign_test([-3, -1, -2, -4, -1, -2])  -> 0.03125  (all 6 agree: the smallest p with 6 pairs)
    sign_test([-3, -1, -2, -4, -1, 2])   -> 0.21875  (5 of 6 agree: not significant)
    sign_test([0, 0])                    -> 1.0
    """
    nonzero_diffs = [diff for diff in pair_diffs if diff != 0]
    if not nonzero_diffs:
        return 1.0
    negatives, count = sum(diff < 0 for diff in nonzero_diffs), len(nonzero_diffs)
    smaller_side = min(negatives, count - negatives)
    tail = sum(math.comb(count, i) for i in range(0, smaller_side + 1)) / 2.0 ** count
    return min(1.0, 2 * tail)


def threshold(decision, a_median):
    """The smallest difference that matters, in the metric's units."""
    setting = decision.get("threshold", 0)
    if isinstance(setting, dict):
        return abs(a_median) * setting["rel"]  # relative: a share of A's median
    return float(setting)


def noise_floor(noise, metric):
    """The A/A comparison's widest paired difference, when it judged the same metric."""
    if not noise or noise.get("metric") != metric or "lo" not in noise:
        return 0.0
    return max(abs(noise["lo"]), abs(noise["hi"]))


def predicted_range(decision, a_median):
    """The prediction as (lo, hi) in the metric's units.

    predict {lo: 15, hi: 25}                      -> (15, 25), whatever A's median
    predict {rel_lo: -0.2, rel_hi: -0.1}, A = 50  -> (-10.0, -5.0)
    """
    prediction = decision.get("predict", {})
    if "lo" in prediction:
        return prediction["lo"], prediction["hi"]
    return prediction["rel_lo"] * abs(a_median), prediction["rel_hi"] * abs(a_median)


def usable_diffs(metric, rows):
    """(pair_diffs, a_values, invalid_pairs, stalled_pairs) from [(a_row, b_row)].

    - invalid: either run failed its gates or left no row.
    - stalled: a body-mean pair whose two runs' trimmed shares differ by more than
      MAX_TRIMMED_SHARE_DIFF.
    """
    trim_column = TRIM_COLUMN.get(metric)
    pair_diffs, a_values, invalid, stalled = [], [], 0, 0
    for a_row, b_row in rows:
        if not (valid(a_row) and valid(b_row)):
            invalid += 1
        elif trim_column and abs((number_or_none(a_row, trim_column) or 0)
                                 - (number_or_none(b_row, trim_column) or 0)) \
                > MAX_TRIMMED_SHARE_DIFF:
            stalled += 1
        elif (number_or_none(a_row, metric) is not None
              and number_or_none(b_row, metric) is not None):
            pair_diffs.append(number_or_none(b_row, metric) - number_or_none(a_row, metric))
            a_values.append(number_or_none(a_row, metric))
    return pair_diffs, a_values, invalid, stalled


def verdict(decision, pair_diffs, a_values, noise):
    """The statistics and verdict for at least MIN_USABLE_PAIRS diffs, as result keys."""
    median, lo, hi = median_or_none(pair_diffs), min(pair_diffs), max(pair_diffs)
    a_median = median_or_none(a_values)
    agree = max(sum(diff < 0 for diff in pair_diffs), sum(diff > 0 for diff in pair_diffs))
    bar = max(threshold(decision, a_median), noise_floor(noise, decision["metric"]))
    sign_p = sign_test(pair_diffs)
    if sign_p <= SIGNIFICANCE_LEVEL and abs(median) >= bar:
        outcome = "B better" if median < 0 else "B worse"
    elif -bar <= lo and hi <= bar:
        outcome = "no difference"
    else:
        outcome = "unresolved"
    predicted_lo, predicted_hi = predicted_range(decision, a_median)
    return dict(a_median=a_median, median=median, lo=lo, hi=hi, agree=agree, sign_p=sign_p, bar=bar,
                verdict=outcome, predicted=[predicted_lo, predicted_hi],
                prediction_hit=predicted_lo <= median <= predicted_hi)


def analyse(decision, out, pairs, noise=None):
    """One comparison's result, as written to result.json.

    - Always: id, a, b, metric, pairs, n (usable pairs), invalid_pairs, stalled_pairs, diffs,
      verdict.
    - Too few usable pairs: why.
    - Otherwise: a_median, median, lo, hi, agree, sign_p, bar (the larger of the threshold and the
      A/A noise floor), predicted [lo, hi], prediction_hit.
    """
    metric = decision["metric"]
    rows = [(read_row(a_dir), read_row(b_dir)) for a_dir, b_dir, _ in run_dirs(out, pairs)]
    pair_diffs, a_values, invalid, stalled = usable_diffs(metric, rows)
    result = {"id": decision["id"], "a": decision["a"], "b": decision["b"], "metric": metric,
              "pairs": pairs, "n": len(pair_diffs), "invalid_pairs": invalid,
              "stalled_pairs": stalled, "diffs": pair_diffs}
    if len(pair_diffs) < MIN_USABLE_PAIRS:
        return dict(result, verdict="unresolved", why="%d usable pairs" % len(pair_diffs))
    result.update(verdict(decision, pair_diffs, a_values, noise))
    return result


def summary(result):
    """The one line printed per comparison."""
    if "median" not in result:
        return "%-16s %-13s %s (%s)" % (result["id"], result["verdict"], result["metric"],
                                        result["why"])
    return ("%-16s %-13s %s: median %+.3g, range [%.3g, %.3g], %d/%d agree, sign p=%.3f, "
            "predicted [%.3g, %.3g] %s" % (
                result["id"], result["verdict"], result["metric"], result["median"], result["lo"],
                result["hi"], result["agree"], result["n"], result["sign_p"],
                result["predicted"][0], result["predicted"][1],
                "hit" if result["prediction_hit"] else "miss"))


def compare(decision, args, noise):
    """Runs (unless --analyse-only) and analyses one comparison, then writes its result.json."""
    out = os.path.join(args.out, decision["id"])
    reruns = ({} if args.analyse_only
              else run_series(decision, args.bin_dir, out, args.pairs, args.duration_ms))
    result = analyse(decision, out, args.pairs, noise)
    result["reruns"] = reruns
    with open(os.path.join(out, "result.json"), "w") as result_file:
        json.dump(result, result_file, indent=1)
    print("  " + summary(result), flush=True)
    return result


def estimated_seconds(decision, args):
    """Rough wall time of one comparison's runs, for the --dry-run plan."""
    if decision["metric"] == FEED_METRIC:
        run_seconds = FEED_RUN_SECONDS  # feed_bench ignores --duration-ms
    else:
        run_seconds = args.duration_ms / 1000 + BUS_RUN_OVERHEAD_SECONDS
    return 2 * args.pairs * run_seconds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("decision", nargs="?", help="one decision id from bench/decisions.yaml")
    parser.add_argument("--all", action="store_true", help="every decision, then the report")
    parser.add_argument("--only", default="", help="with --all: comma-separated decision ids")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--analyse-only", action="store_true",
                        help="no runs; recompute from the rows on disk")
    parser.add_argument("--bin-dir", default="build/bench")
    parser.add_argument("--out", default="results/raw")
    parser.add_argument("--pairs", type=int, default=DEFAULT_PAIRS)
    parser.add_argument("--duration-ms", type=int, default=DEFAULT_DURATION_MS)
    parser.add_argument("--cooldown", type=float, default=DEFAULT_COOLDOWN_SECONDS,
                        help="seconds between comparisons, so heat does not carry over")
    args = parser.parse_args(argv)
    decisions = load_decisions()
    by_id = {decision["id"]: decision for decision in decisions}
    if not args.all:
        if args.decision not in by_id:
            parser.error("name a decision (%s) or pass --all" % ", ".join(by_id))
        decision = by_id[args.decision]
        noise = None
        if "noise" in decision:
            noise_path = os.path.join(args.out, decision["noise"], "result.json")
            if os.path.exists(noise_path):
                with open(noise_path) as noise_file:
                    noise = json.load(noise_file)
        result = compare(decision, args, noise)
        return 0 if "median" in result else 3  # 3: too few usable pairs for a verdict

    only = set(filter(None, args.only.split(",")))
    selected = [decision for decision in decisions if not only or decision["id"] in only]
    # The A/A noise floors first: the other comparisons read them.
    plan = ([decision for decision in selected if decision["id"].startswith("AA-")] +
            [decision for decision in selected if not decision["id"].startswith("AA-")])
    total = sum(estimated_seconds(decision, args) + args.cooldown for decision in plan)
    print("%d comparisons, %d pairs each, about %.0f min" % (len(plan), args.pairs, total / 60))
    for decision in plan:
        print("  %-16s %-8s -> %-10s %s"
              % (decision["id"], decision["a"], decision["b"], decision["metric"]))
    if args.dry_run:
        return 0
    binaries = {binary(args.bin_dir, variant)
                for decision in plan for variant in (decision["a"], decision["b"])}
    missing = sorted(path for path in binaries if not os.access(path, os.X_OK))
    if missing:
        sys.exit("compare: missing binaries (build first): %s" % ", ".join(missing))
    results = {}
    for index, decision in enumerate(plan):
        print("%s: %s -> %s on %s"
              % (decision["id"], decision["a"], decision["b"], decision["metric"]), flush=True)
        results[decision["id"]] = compare(decision, args, results.get(decision.get("noise")))
        if index + 1 < len(plan) and not args.analyse_only:
            time.sleep(args.cooldown)
    return report.main([os.path.dirname(os.path.abspath(args.out))])


if __name__ == "__main__":
    sys.exit(main())
