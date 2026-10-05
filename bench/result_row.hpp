#pragma once

// A run's record and its validity gates.
// - ResultRow: one CSV row per run, written to DIR/row.csv; compare.py and report.py read it.
// - The gates: add_failed_gate() collects failed gate names; finish_row() sets valid = 0 if any
//   failed, so a run with a failed gate is written but not counted.
// - format_ticks() and print_conditions_line(): the few summary lines for the person at the
//   terminal.
// - Used by bus_bench, dispatch_bench and feed_bench.

#include <errno.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mdbus/status.hpp"
#include "measurement.hpp"
#include "timing.hpp"

namespace mdbus::bench {

// The CPU gates judge a run by what it measured, not by machine settings that might matter.
// - A P-core role whose effective clock (cycles / on-CPU time) fell below kMinPCoreGhz was
//   throttled, whatever the cause (heat, Low Power Mode, the power source).
// - On this M1 the P-cores measured just over 3 GHz with one core busy and a little lower with
//   four, so 2.8 GHz leaves room for four busy roles and still catches throttling.
// - A P-core role must also have spent at least kMinPCoreShare of its cycles on the P-cores.
constexpr double kMinPCoreGhz = 2.8;
constexpr double kMinPCoreShare = 0.99;

// A Debug build is refused (gate "env:debug_build"): it would change several variables at once.
inline bool is_release_build() {
#ifdef NDEBUG
  return true;
#else
  return false;
#endif
}

// Adds gate_name to the |-separated failed gates when the gate did not pass.
inline void add_failed_gate(std::string& failed_gates, bool passed, const std::string& gate_name) {
  if (passed) return;
  if (!failed_gates.empty()) failed_gates += "|";
  failed_gates += gate_name;
}

// The two gates every P-core role must pass: it ran on the P-cores, at full clock.
inline void add_cpu_gates(std::string& failed_gates, const ProcessCounters& counters) {
  add_failed_gate(failed_gates, counters.p_core_share() >= kMinPCoreShare, "pshare");
  add_failed_gate(failed_gates, counters.effective_ghz() >= kMinPCoreGhz, "ghz");
}

// One CSV row per run: a header line and a value line, columns in the order first set.
class ResultRow {
 private:
  std::vector<std::string> keys;    // column names, in the order first set
  std::vector<std::string> values;  // values[i] belongs to keys[i]

 public:
  // Six significant digits ("%.6g"): more than any measurement here resolves.
  void set_number(const std::string& key, double value) {
    char formatted[32];  // "%.6g" of a double needs at most 13 characters
    std::snprintf(formatted, sizeof formatted, "%.6g", value);
    put(key, formatted);
  }

  void set_integer(const std::string& key, std::uint64_t value) {
    put(key, std::to_string(value));
  }

  void set_text(const std::string& key, const std::string& value) {
    put(key, value);
  }

  // Writes dir/row.csv. Creates dir, but not its parents: compare.py and CMake create those.
  bool write(const std::string& dir) const {
    constexpr mode_t kDirMode = 0755;  // rwxr-xr-x
    if (mkdir(dir.c_str(), kDirMode) != 0 && errno != EEXIST) return false;

    std::string header_line;
    std::string value_line;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i > 0) {
        header_line += ",";
        value_line += ",";
      }
      header_line += keys[i];
      value_line += values[i];
    }

    std::FILE* file = std::fopen((dir + "/row.csv").c_str(), "w");
    if (file == nullptr) return false;
    const bool printed =
        std::fprintf(file, "%s\n%s\n", header_line.c_str(), value_line.c_str()) > 0;
    const bool closed = std::fclose(file) == 0;
    return printed && closed;
  }

  // The value of column key, or "" if it was never set.
  std::string text(const std::string& key) const {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == key) return values[i];
    }
    return "";
  }

  double number(const std::string& key) const {
    return std::strtod(text(key).c_str(), nullptr);
  }

  bool has_column(const std::string& key) const {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
  }

 private:
  // Adds a column; each is set once. A comma would split the CSV field, so it becomes a
  // semicolon.
  void put(const std::string& key, std::string value) {
    check_or_abort(!has_column(key), "ResultRow: a column is set twice");
    std::replace(value.begin(), value.end(), ',', ';');
    keys.push_back(key);
    values.push_back(value);
  }
};

// Sets the row's last three columns (valid, reject_reason, aborted), then writes dir/row.csv.
// - valid = 1 only when no gate failed and the run was not aborted; otherwise valid = 0 and the
//   scripts leave the row out.
// - False if the row could not be written.
inline bool finish_row(ResultRow& row, const std::string& failed_gates, const std::string& dir,
                       const std::string& aborted = "") {
  const bool valid = failed_gates.empty() && aborted.empty();
  row.set_integer("valid", valid ? 1 : 0);
  row.set_text("reject_reason", failed_gates);
  row.set_text("aborted", aborted);
  return row.write(dir);
}

// Below this many ticks a reading is shown as an interval (see format_ticks).
constexpr double kTicksShownAsInterval = 3;
// At or above 10 us a reading is shown in us instead of ns.
constexpr double kNsShownAsMicroseconds = 1e4;
constexpr double kNsPerMicrosecond = 1e3;

// A latency in ticks as text for the terminal, the same way scripts/report.py shows it.
// - Below 3 ticks a reading of n ticks only bounds the true value to (n - 1, n + 1) ticks, so it
//   is shown as that interval.
inline std::string format_ticks(double ticks) {
  char text[48];  // the longest form, the interval, is under 40 characters
  const double ns = ticks * kNsPerTick;
  if (ticks < kTicksShownAsInterval) {
    const double rounded_ticks = std::round(ticks);
    std::snprintf(text, sizeof text, "%.0f t, in (%.0f, %.0f) ns", rounded_ticks,
                  std::max(0.0, rounded_ticks - 1) * kNsPerTick, (rounded_ticks + 1) * kNsPerTick);
  } else if (ns < kNsShownAsMicroseconds) {
    std::snprintf(text, sizeof text, "%.0f ns", ns);
  } else {
    std::snprintf(text, sizeof text, "%.1f us", ns / kNsPerMicrosecond);
  }
  return text;
}

// The summary's last line: whether the run was clean, and if not, why.
inline void print_conditions_line(const ResultRow& row) {
  const std::string failed_gates = row.text("reject_reason");
  const std::string aborted = row.text("aborted");
  if (!aborted.empty()) {
    std::printf("  conditions: run aborted (%s)\n", aborted.c_str());
  } else if (row.text("valid") == "1") {
    std::printf("  conditions: clean\n");
  } else {
    std::printf("  conditions: indicative only, gates failed: %s\n", failed_gates.c_str());
  }
}

}  // namespace mdbus::bench
