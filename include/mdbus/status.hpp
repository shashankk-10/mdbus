#pragma once

// Error codes and the one fatal check.
// - Status: what BusWriter::open, BusReader::open, Consumer::attach and the shared-memory calls
//   return. The library never throws; after SystemCallFailed, errno still holds the cause.
// - check_or_abort: for conditions that mean a bug, not a runtime error.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace mdbus {

// Why a cold-path call (open, attach, create) failed. Only Ok means success.
// - BadName: bus name empty, over 24 characters, or has a character other than letters,
//   digits, '_' and '-' (e.g. "my.bus").
// - BadArgument: BusWriter::open with 0 or more than 65536 instruments.
// - AnotherWriterRunning: a second `mdbus_feed_handler --bus demo` while the first is alive or
//   stopped (it holds the lock).
// - NoSuchBus: no shm object with that name (no feed handler yet).
// - NotReady: the object exists but the writer has not sized it or not stored the ready marker
//   yet, or a newer writer is replacing it. Readers retry this one.
// - NotABusSegment: the object is too small, or its ready marker is some other value.
// - LayoutMismatch: reader and writer were compiled with different BusLayouts (e.g. 1024 vs
//   16384 slots).
// - SizeMismatch: the stored instrument count is 0 or over 65536, or the mapping is smaller
//   than that count needs.
// - AlreadyExists: exclusive create found the name taken.
// - SystemCallFailed: see errno.
enum class Status : std::uint8_t {
  Ok,
  BadName,
  BadArgument,
  AnotherWriterRunning,
  NoSuchBus,
  NotReady,
  NotABusSegment,
  LayoutMismatch,
  SizeMismatch,
  AlreadyExists,
  SystemCallFailed,
};

// The value's name, for error messages (the programs print it instead of a bare number).
// Example:
//   status_name(Status::NoSuchBus)              -> "NoSuchBus"
//   status_name(Status::AnotherWriterRunning)   -> "AnotherWriterRunning"
//   status_name(static_cast<Status>(42))        -> "unknown status"
inline const char* status_name(Status status) {
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::BadName:
      return "BadName";
    case Status::BadArgument:
      return "BadArgument";
    case Status::AnotherWriterRunning:
      return "AnotherWriterRunning";
    case Status::NoSuchBus:
      return "NoSuchBus";
    case Status::NotReady:
      return "NotReady";
    case Status::NotABusSegment:
      return "NotABusSegment";
    case Status::LayoutMismatch:
      return "LayoutMismatch";
    case Status::SizeMismatch:
      return "SizeMismatch";
    case Status::AlreadyExists:
      return "AlreadyExists";
    case Status::SystemCallFailed:
      return "SystemCallFailed";
  }
  return "unknown status";
}

// Like assert, but also active in Release builds (assert vanishes under -DNDEBUG).
// - Used on cold paths and on a few hot-path invariants that cost one predictable branch.
// - Why abort and not a Status: a false condition here is a bug in the caller, and carrying on
//   would publish or read garbage.
// Example:
//   check_or_abort(instrument_id < 1024, "instrument id out of range"), instrument_id 2000
//   -> stderr "mdbus: check failed: instrument id out of range", then SIGABRT
inline void check_or_abort(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "mdbus: check failed: %s\n", message);
  std::abort();
}

}  // namespace mdbus
