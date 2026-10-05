#pragma once

// Command-line pieces shared by mdbus_exchange_sim, mdbus_feed_handler and mdbus_watch.
// - Exit codes, and strict number parsing for "--name value" options.
// - The simulator and the feed handler both take --seed and --instruments: the feed handler
//   derives the same instrument list (symbols and reference prices) from them as the simulator
//   streams from, so nothing about the instruments goes over the wire.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "sim/order_event_generator.hpp"

namespace mdbus {

// Exit codes shared by the three programs.
constexpr int kExitOk = 0;
constexpr int kExitSendFailed = 1;  // mdbus_exchange_sim: a send failed
constexpr int kExitBadCommandLine = 2;
constexpr int kExitSetupFailed = 3;  // a socket or the bus could not be opened

// Parses the whole text as an unsigned decimal number; value is written only on success.
// - Why not strtoull alone: it accepts leading spaces, a minus sign (wrapping "-1" to 2^64-1)
//   and trailing characters, and saturates on overflow. Only the overflow is left to it (ERANGE).
// Example:
//   "42"   -> true, value 42
//   "abc", "", "-1", " 7", "7x", "99999999999999999999" -> false, value untouched
inline bool parse_unsigned(const std::string& text, std::uint64_t& value) {
  if (text.empty() || text[0] < '0' || text[0] > '9') return false;
  char* end = nullptr;
  errno = 0;
  const std::uint64_t parsed = std::strtoull(text.c_str(), &end, 10);
  if (*end != '\0' || errno != 0) return false;
  value = parsed;
  return true;
}

// A UDP port, 1..65535.
// Example: "30001" -> true, port 30001;  "0", "70000" -> false, port untouched
inline bool parse_port(const std::string& text, std::uint16_t& port) {
  std::uint64_t number = 0;
  if (!parse_unsigned(text, number) || number == 0 || number > UINT16_MAX) return false;
  port = static_cast<std::uint16_t>(number);
  return true;
}

// Stores a --seed or --instruments value into config. False for any other key, or a bad value.
// Example:
//   ("--seed", "7")             -> true, config.seed 7
//   ("--instruments", "65535")  -> false (65535 is kNoInstrument)
//   ("--port", "1")             -> false (a key it does not own)
inline bool parse_seed_or_instruments(const std::string& key, const std::string& value,
                                      book::GeneratorConfig& config) {
  std::uint64_t number = 0;
  if (!parse_unsigned(value, number)) return false;
  if (key == "--seed") {
    config.seed = number;
    return true;
  }
  if (key == "--instruments") {
    // Ids are 16-bit; 0xFFFF (book::kNoInstrument) means no instrument.
    if (number == 0 || number >= book::kNoInstrument) return false;
    config.instrument_count = static_cast<std::uint32_t>(number);
    return true;
  }
  return false;
}

}  // namespace mdbus
