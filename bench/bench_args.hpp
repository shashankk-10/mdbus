#pragma once

// Command-line options for the benchmark programs, and the exit codes they share.
// - Every option is a `--key value` pair; there are no bare flags.
// - Each program states the rules its values must keep in kCommandLineRules. A command line
//   that breaks one is refused before anything runs: the program prints why and its usage,
//   writes no row, and exits with kExitRunFailed. Broken means a key without a value, a value
//   that is not a plain decimal number in its option's range, or one that is not among its
//   choices.
// - A key a program does not use is ignored, not refused: CMake's smoke runs and compare.py pass
//   every benchmark the same few.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "src/command_line.hpp"  // parse_unsigned: the programs' own strict number rule

namespace mdbus::bench {

// Exit codes. 0: the run completed and wrote its row, whatever its gates said.
constexpr int kExitRunFailed = 2;        // a refused command line, an aborted run, or no row
constexpr int kExitMeasuredNothing = 4;  // --smoke: the run measured nothing

// number() reads a value through a double, which holds every whole number up to 2^53 exactly.
constexpr std::uint64_t kMaxWholeNumber = std::uint64_t{1} << 53;
// The longest --duration-ms or --warmup-ms: together they stay under the 71 minutes the
// launcher's usleep can sleep (it takes 32-bit microseconds).
constexpr std::uint64_t kMaxPhaseMs = 30 * 60 * 1000;

// What one option's value must be: one of choices ("off|read"), or, with no choices, a
// whole decimal number in [min_value, max_value].
struct OptionRule {
  const char* key;
  std::uint64_t min_value = 0;
  std::uint64_t max_value = kMaxWholeNumber;
  const char* choices = nullptr;
};

// A program's rules, and the usage line a refusal prints.
struct CommandLineRules {
  const char* usage;
  std::span<const OptionRule> options;
};

// Defined by each benchmark program.
extern const CommandLineRules kCommandLineRules;

// Whether text is one of the |-separated choices.
inline bool is_one_of(const char* text, const char* choices) {
  const std::string bounded_choices = std::string("|") + choices + "|";
  return std::strchr(text, '|') == nullptr &&
         bounded_choices.find(std::string("|") + text + "|") != std::string::npos;
}

// Prints why the command line is refused, and the usage, then exits with kExitRunFailed.
[[noreturn]] inline void refuse_command_line(const char* program, const std::string& why) {
  std::fprintf(stderr, "%s: %s\nusage: %s %s\n", program, why.c_str(), program,
               kCommandLineRules.usage);
  std::exit(kExitRunFailed);
}

// Refuses argv unless everything after the program name is `--key value` pairs and every value
// keeps its key's rule. No value starts with "--", so one that does is the next key.
inline void check_command_line(int argc, char** argv) {
  for (int key_index = 1; key_index < argc; key_index += 2) {
    const char* key = argv[key_index];
    if (std::strncmp(key, "--", 2) != 0) {
      refuse_command_line(argv[0], std::string(key) + " is not a --key");
    }
    if (key_index + 1 == argc || std::strncmp(argv[key_index + 1], "--", 2) == 0) {
      refuse_command_line(argv[0], std::string(key) + " has no value");
    }
    const char* value = argv[key_index + 1];
    for (const OptionRule& rule : kCommandLineRules.options) {
      if (std::strcmp(key, rule.key) != 0) continue;
      std::uint64_t number = 0;
      const bool kept = rule.choices != nullptr
                            ? is_one_of(value, rule.choices)
                            : parse_unsigned(value, number) && number >= rule.min_value &&
                                  number <= rule.max_value;
      if (kept) continue;
      const std::string expected = rule.choices != nullptr
                                       ? rule.choices
                                       : "a whole number from " + std::to_string(rule.min_value) +
                                             " to " + std::to_string(rule.max_value);
      refuse_command_line(argv[0], std::string(key) + " " + value + ": expected " + expected);
    }
  }
}

// A view over argv that looks up `--key value` pairs.
struct BenchArgs {
  int argc;
  char** argv;

  // The value after the last `key` in argv, or default_value if key is absent.
  // - Scans from the end, so a later occurrence wins: roles inherit the launcher's argv and
  //   append their own.
  // - Checks the whole command line first, so no unchecked value reaches number(). The check
  //   sits in this out-of-line lookup, not in main: bus_bench's main inlines the measured roles,
  //   and code added there changes their machine code.
  [[gnu::noinline]] std::string text(const char* key, const char* default_value) const {
    check_command_line(argc, argv);
    for (int key_index = argc - 2; key_index >= 1; key_index -= 2) {
      if (std::strcmp(argv[key_index], key) == 0) return argv[key_index + 1];
    }
    return default_value;
  }

  // text() parsed with strtod. Every key a program reads this way has a rule, so the text is a
  // whole number in range and the program's casts of it are exact.
  // Example: --rate 5000000 --rate 2000000 -> 2e6 (the last wins); no --rate -> default_value
  double number(const char* key, double default_value) const {
    const std::string value = text(key, "");
    if (value.empty()) return default_value;
    return std::strtod(value.c_str(), nullptr);
  }
};

}  // namespace mdbus::bench
