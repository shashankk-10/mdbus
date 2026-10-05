#!/usr/bin/env python3
"""Regression tests for the benchmark tooling, one per bug it once had.

  bench/bench_tools_test.py [BIN_DIR]      BIN_DIR: the built benchmarks (default build/bench)

compare.py and report.py run on scratch trees, with stub binaries where a run is needed; the
compare.py tests need PyYAML and are skipped without it. The program tests run BIN_DIR's
benchmarks, and are skipped when there are none.
"""
import contextlib
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "scripts"))
import report  # noqa: E402
try:
    import compare  # noqa: E402
except ImportError:  # no PyYAML
    compare = None

BIN_DIR = os.path.abspath(os.path.join(REPO, "build", "bench"))
if __name__ == "__main__" and len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    BIN_DIR = os.path.abspath(sys.argv.pop(1))
EXIT_RUN_FAILED = 2  # bench/bench_args.hpp


def write_row(run_dir, hop_ns):
    """A valid row.csv, as a benchmark writes one."""
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(run_dir, "row.csv"), "w") as row_file:
        row_file.write("variant,hop_mean_body_ns,hop_trim_frac,valid\nBase,%s,0,1\n" % hop_ns)


def read_file(path):
    with open(path, "rb") as opened:
        return opened.read()


class ScriptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bench_tools_")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_report_never_replaces_a_report_with_a_smaller_one(self):
        # A clone's results/ holds report.md and runs.csv but not the rows behind them, so
        # report.py must refuse to rewrite them as empty tables. One comparison under it (a live
        # compare.py Copy) is still fewer than the committed report counts.
        results = os.path.join(self.tmp, "results")
        shutil.copytree(os.path.join(REPO, "results"), results)
        committed = [read_file(os.path.join(results, name)) for name in ("report.md", "runs.csv")]
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(report.main([results]), 0)
            os.makedirs(os.path.join(results, "raw", "Copy"))
            with open(os.path.join(results, "raw", "Copy", "result.json"), "w") as result_file:
                json.dump({"id": "Copy", "a": "Base", "b": "Copy15", "metric": "hop_mean_body_ns",
                           "verdict": "unresolved"}, result_file)
            self.assertNotEqual(report.main([results]), 0)
        self.assertEqual(
            [read_file(os.path.join(results, name)) for name in ("report.md", "runs.csv")],
            committed)

    @unittest.skipUnless(compare, "compare.py needs PyYAML")
    def test_noise_floor_is_robust_to_stalled_pairs(self):
        # AA-e2e-p99 with two stalled pairs: the widest A/A difference is 6146 ticks, and
        # Mutex-Base-e2e's 6/6-agreeing -5240 ticks must still read "B better".
        noise = {"metric": "e2e_p99", "lo": -6146.0, "hi": 1.0,
                 "diffs": [-3931.0, 1.0, -6146.0, 0.0, 0.0, 0.0]}
        decision = {"id": "Mutex-Base-e2e", "metric": "e2e_p99", "threshold": 1,
                    "predict": {"lo": -51, "hi": -34}}
        diffs = [-5240.0, -5100.0, -6030.0, -5060.0, -5300.0, -5200.0]
        result = compare.verdict(decision, diffs, [5251.0] * 6, noise)
        self.assertEqual(result["verdict"], "B better")
        # Pad-pp: a range inside the A/A's own spread is "no difference", though it is wider
        # than the A/A's median.
        noise = {"metric": "hop_mean_body_ns", "lo": -7.723, "hi": 5.354,
                 "diffs": [5.354, -1.397, -7.723, 3.65]}
        decision = {"id": "Pad-pp", "metric": "hop_mean_body_ns", "threshold": 1,
                    "predict": {"lo": -2, "hi": 2}}
        diffs = [-1.345, -4.71, -4.432, 0.654, -0.671, 2.097]
        result = compare.verdict(decision, diffs, [88.0] * 6, noise)
        self.assertEqual(result["verdict"], "no difference")

    @unittest.skipUnless(compare, "compare.py needs PyYAML")
    def test_analyse_only_on_a_fresh_out_reports_there(self):
        # --analyse-only needs no built binaries and works on a fresh --out, and the report goes
        # into --out itself, not its parent (which would count every row.csv beside it).
        out = os.path.join(self.tmp, "raw")
        write_row(os.path.join(self.tmp, "b"), 90)  # an unrelated run beside --out
        with contextlib.redirect_stdout(io.StringIO()):
            code = compare.main(["--all", "--only", "Copy", "--analyse-only", "--out", out,
                                 "--bin-dir", os.path.join(self.tmp, "none")])
        self.assertEqual(code, 0)
        self.assertIn(b"Runs: 0 (0 valid). Comparisons: 1.",
                      read_file(os.path.join(out, "report.md")))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "report.md")))

    @unittest.skipUnless(compare, "compare.py needs PyYAML")
    def test_rows_older_than_their_binary_run_again(self):
        bin_dir, calls = os.path.join(self.tmp, "bin"), os.path.join(self.tmp, "calls")
        os.makedirs(bin_dir)
        for variant in ("Base", "Copy15"):  # stubs that log the call and write a valid row
            stub = os.path.join(bin_dir, "mdbus_bench_" + variant)
            with open(stub, "w") as stub_file:
                stub_file.write('#!/bin/sh\necho "$@" >> %s\n' % calls +
                                'while [ "$1" != --out ]; do shift; done\nmkdir -p "$2"\n'
                                'printf "hop_mean_body_ns,valid\\n80,1\\n" > "$2/row.csv"\n')
            os.chmod(stub, 0o755)
        out = os.path.join(self.tmp, "raw", "Copy")
        for pair, mtime in ((0, time.time() - 3600), (1, time.time() + 3600)):  # 0 is stale
            for side in "AB":
                run_dir = os.path.join(out, "p%02d_%s" % (pair, side))
                write_row(run_dir, 80)
                os.utime(os.path.join(run_dir, "row.csv"), (mtime, mtime))
        decision = next(d for d in compare.load_decisions() if d["id"] == "Copy")
        with contextlib.redirect_stdout(io.StringIO()):
            compare.run_series(decision, bin_dir, out, 2, 100)
        ran = read_file(calls).decode()
        self.assertEqual((ran.count("/p00_"), ran.count("/p01_")), (2, 0))


def binary(name):
    return os.path.join(BIN_DIR, "mdbus_bench_" + name)


def run_bounded(command, timeout_s, **kwargs):
    """subprocess.run in a session of its own; on a timeout, kills every process it started and
    returns None."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               start_new_session=True, **kwargs)
    try:
        stdout, stderr = process.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        return None
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def shm_exists(name):
    import _posixshmem
    try:
        os.close(_posixshmem.shm_open(name, os.O_RDONLY, mode=0))
        return True
    except FileNotFoundError:
        return False


def role_pids(bus_name):
    listed = subprocess.run(["pgrep", "-f", "--", "--name %s( |$)" % bus_name],
                            capture_output=True, text=True).stdout.split()
    return [int(pid) for pid in listed]


def wait_for(condition, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline and not condition():
        time.sleep(0.02)
    return condition()


@unittest.skipUnless(os.access(binary("Base"), os.X_OK), "no built benchmarks in " + BIN_DIR)
class ProgramTest(unittest.TestCase):
    def test_bad_command_lines_are_refused_without_a_row(self):
        # Each of these once ran a benchmark and wrote a row: a negative number was undefined
        # behaviour, --help and a misspelt choice measured the defaults, --events 0 timed
        # nothing, and --live -1 aborted.
        cases = [("Base", ["--duration-ms", "-5"]), ("Base", ["--rate", "1e6"]),
                 ("Base", ["--slow", "Read"]), ("Base", ["--help"]),
                 ("dispatch", ["--dispatch", "bogus"]), ("F4", ["--events", "0"]),
                 ("F4", ["--live", "-1"])]
        with tempfile.TemporaryDirectory() as out:
            for name, args in cases:
                with self.subTest(binary=name, args=args):
                    run = run_bounded([binary(name)] + args + ["--out", out], 10)
                    self.assertEqual(run.returncode, EXIT_RUN_FAILED)
                    self.assertIn("usage:", run.stderr)
                    self.assertFalse(os.path.exists(os.path.join(out, "row.csv")))


@unittest.skipUnless(os.access(binary("Base"), os.X_OK), "no built benchmarks in " + BIN_DIR)
class KilledLauncherTest(unittest.TestCase):
    """A launcher killed with SIGKILL runs no cleanup of its own."""

    def start(self, *args):
        self.tmp = tempfile.mkdtemp(prefix="bench_tools_kill_")
        self.launcher = subprocess.Popen([binary("Base")] + list(args) + ["--out", self.tmp],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.bus = "bk%d" % self.launcher.pid
        self.names = ["/mdb.%s.d" % self.bus, "/mdb.%s.c" % self.bus, "/mdb.%sr.d" % self.bus]
        self.assertTrue(wait_for(lambda: role_pids(self.bus), 5), "no role started")

    def kill_launcher(self):
        self.launcher.kill()
        self.launcher.wait()

    def tearDown(self):  # whatever the outcome, leave nothing behind
        self.kill_launcher()
        for pid in role_pids(self.bus):
            os.kill(pid, signal.SIGKILL)
        import _posixshmem
        for name in self.names:
            with contextlib.suppress(FileNotFoundError):
                _posixshmem.shm_unlink(name)
        for lock in (self.bus + ".lock", self.bus + "r.lock"):
            with contextlib.suppress(FileNotFoundError):
                os.unlink(os.path.join("/var/tmp/mdbus-%d" % os.geteuid(), lock))
        shutil.rmtree(self.tmp)

    def test_no_segment_name_outlives_a_started_run(self):
        # Once every role is ready the launcher removes the names, long before this 20 s run
        # ends, so a launcher killed from then on leaks no segment.
        self.start("--duration-ms", "20000", "--warmup-ms", "100")
        self.assertTrue(wait_for(lambda: not any(shm_exists(name) for name in self.names), 5),
                        "segment names still there while the run goes on")
        self.assertIsNone(self.launcher.poll())

    def test_roles_of_a_killed_launcher_exit(self):
        # Killed during start-up or the short window: every role must exit by itself, whether it
        # was waiting for go or heartbeating for the readers to drain.
        self.start("--duration-ms", "500", "--warmup-ms", "100")
        time.sleep(0.1)
        self.kill_launcher()
        self.assertTrue(wait_for(lambda: not role_pids(self.bus), 15),
                        "roles still running: %s" % role_pids(self.bus))


if __name__ == "__main__":
    unittest.main()
