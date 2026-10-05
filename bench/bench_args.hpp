#pragma once

// Command-line options for the benchmark programs.
// - Every option is a `--key value` pair; there are no bare flags.
// - Used by bus_bench (launcher and roles), dispatch_bench and feed_bench.

#include <cstdlib>
#include <cstring>
#include <string>

namespace mdbus::bench {

// A view over argv that looks up `--key value` pairs.
struct BenchArgs {
  int argc;
  char** argv;

  // The value after the last `key` in argv, or default_value if key is absent.
  // - Scans from the end, so a later occurrence wins: roles inherit the launcher's argv and
  //   append their own.
  std::string text(const char* key, const char* default_value) const {
    for (int arg_index = argc - 2; arg_index >= 1; --arg_index) {
      if (std::strcmp(argv[arg_index], key) == 0) return argv[arg_index + 1];
    }
    return default_value;
  }

  // text() parsed with strtod.
  // Example:
  //   --rate 5e6 --rate 2e6  -> 2e6 (last occurrence wins)
  //   no --rate              -> default_value
  //   --rate abc             -> 0, not default_value: a trap the caller must check
  double number(const char* key, double default_value) const {
    const std::string value = text(key, "");
    if (value.empty()) return default_value;
    return std::strtod(value.c_str(), nullptr);
  }
};

}  // namespace mdbus::bench
