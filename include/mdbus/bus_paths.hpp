#pragma once

// A bus's name rules and the two paths derived from its name.
// - A bus is found by name: the writer and every reader turn "demo" into the same shm name
//   (the segment) and the same lock-file path (the writer lock) with make_bus_paths().
// - Used by BusWriter, BusReader and WriterHealthMonitor; mdbus_watch --destroy calls
//   destroy_bus().

#include <sys/mman.h>
#include <unistd.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "mdbus/constants.hpp"

namespace mdbus {

// Every shm object mdbus creates is named kShmNamePrefix + a name + a suffix.
constexpr const char* kShmNamePrefix = "/mdb.";

// The bus segment's suffix. It keeps the segment apart from bus_bench's control segment
// "/mdb.<name>.c", which is also why '.' is not allowed in a bus name.
constexpr const char* kSegmentNameSuffix = ".d";

constexpr std::size_t kMaxBusNameLength = 24;  // 31 - 5 for "/mdb." - 2 for ".d"
static_assert(std::string_view(kShmNamePrefix).size() + kMaxBusNameLength +
                      std::string_view(kSegmentNameSuffix).size() <=
                  kMaxShmNameLength,
              "the longest bus name must still give a valid shm name");

// + the user id. Not /tmp: macOS deletes /tmp files untouched for 3 days, even while a writer
// holds the lock, and a second writer could then lock a fresh file of the same name.
constexpr const char* kLockDirectoryPrefix = "/var/tmp/mdbus-";

// 1 to 24 characters, each a letter, a digit, '_' or '-'.
inline bool is_valid_bus_name(const std::string& name) {
  if (name.empty() || name.size() > kMaxBusNameLength) return false;
  for (char character : name) {
    const bool allowed = std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                         character == '_' || character == '-';
    if (!allowed) return false;
  }
  return true;
}

// The names a bus is reached by; every process derives the same ones from the bus name.
struct BusPaths {
  std::string segment_name;    // shm_open name of the bus segment
  std::string lock_directory;  // per-user directory holding the lock files
  std::string lock_file;       // the file the writer holds a flock on
};

// Fills `paths` from a bus name, or returns false if the name breaks the rules above.
// Example (user id 501):
//   "demo"    -> true: segment_name "/mdb.demo.d", lock_directory "/var/tmp/mdbus-501",
//                lock_file "/var/tmp/mdbus-501/demo.lock"
//   "my.bus"  -> false ('.' is not allowed)
//   25 chars  -> false (24 is the most; it gives a 31-char segment name)
inline bool make_bus_paths(const std::string& bus_name, BusPaths& paths) {
  if (!is_valid_bus_name(bus_name)) return false;
  paths.segment_name = kShmNamePrefix + bus_name + kSegmentNameSuffix;
  paths.lock_directory = kLockDirectoryPrefix + std::to_string(geteuid());
  paths.lock_file = paths.lock_directory + "/" + bus_name + ".lock";
  return true;
}

// Removes a bus's segment and lock file.
// - Only for a name no process will use again: unlinking a lock file that a live writer holds
//   lets the next writer lock a new file of the same name, and then two writers run at once.
inline void destroy_bus(const std::string& bus_name) {
  BusPaths paths;
  if (!make_bus_paths(bus_name, paths)) return;
  shm_unlink(paths.segment_name.c_str());
  ::unlink(paths.lock_file.c_str());
}

}  // namespace mdbus
