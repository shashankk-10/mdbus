#pragma once

// Command-line pieces shared by mdbus_exchange_sim, mdbus_feed_handler and mdbus_watch: exit
// codes, strict number parsing, and the option loop of the two feed programs.

#include <netinet/in.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "mdbus/feed/multicast_socket.hpp"

namespace mdbus {

// Exit codes shared by the three programs.
constexpr int kExitOk = 0;
constexpr int kExitSendFailed = 1;  // mdbus_exchange_sim: a send failed
constexpr int kExitBadCommandLine = 2;
constexpr int kExitSetupFailed = 3;  // a socket or the bus could not be opened (or locked)

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

// A UDP port, 1..65535; port is written only on success.
inline bool parse_port(const std::string& text, std::uint16_t& port) {
  std::uint64_t number = 0;
  if (!parse_unsigned(text, number) || number == 0 || number > UINT16_MAX) return false;
  port = static_cast<std::uint16_t>(number);
  return true;
}

// The command line of the two feed programs, mdbus_exchange_sim and mdbus_feed_handler.
// - Every option is a "--name value" pair, and both take the multicast group the feed travels
//   on: --group ADDR --port P, both required.
// - Reads --group and --port itself and hands every other pair to parse_option(key, value),
//   which takes the program's own keys and is false for any other key or a bad value.
// - False if an option lacks its value, a pair is refused, or the group is missing or not a
//   multicast address.
template <class ParseOption>
bool parse_feed_command_line(int argc, char** argv, sockaddr_in& group_address,
                             ParseOption&& parse_option) {
  if (argc % 2 == 0) return false;  // an option without its value
  std::string group_text;
  std::uint16_t port = 0;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    bool option_ok = true;
    if (key == "--group") {
      group_text = value;
    } else if (key == "--port") {
      option_ok = parse_port(value, port);
    } else {
      option_ok = parse_option(key, value);
    }
    if (!option_ok) return false;
  }
  return port != 0 && make_multicast_address(group_text, port, group_address);
}

}  // namespace mdbus
