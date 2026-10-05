#pragma once

// Per-instrument recovery snapshots: the snapshot, its 128 B seqlock record, and the table.
// - Publisher::publish_and_update_snapshot writes an instrument's record right after each slot
//   that changes it. Consumer reads one when it lost messages (a lap) and the instrument is stale.
// - The table sits after the ring in the segment, one record per instrument (segment_format.hpp).
// - seqlock: a version counter the writer makes odd while it writes and even when done; a reader
//   copies, then re-reads the counter, and retries if it changed.
// - The record's version is a plain seqlock counter (4 -> 5 -> 6), not the ring's 2*seq+1 /
//   2*seq+2 stamp: a reader asks for "the latest snapshot", not for one particular seq.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/atomic_policy.hpp"
#include "mdbus/constants.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/wait_policy.hpp"

namespace mdbus {

// One instrument's top levels, enough for a reader to rebuild its view of the book.
// - bids and asks are best first; only the first bid_count / ask_count entries are real.
// - last_included_seq: the newest message whose effect the snapshot includes, so the reader
//   drops ring messages up to it and delivers the ones after.
struct InstrumentSnapshot {
  std::uint64_t last_included_seq;
  std::uint64_t exchange_time_ns;
  std::uint16_t instrument_id;
  std::uint8_t bid_count;
  std::uint8_t ask_count;
  std::uint32_t instrument_flags;  // InstrumentFlag bits (messages.hpp)
  Level bids[kTopLevelsPerSide];
  Level asks[kTopLevelsPerSide];
};
constexpr std::size_t kSnapshotWords = 15;  // 120 B / 8 B
static_assert(sizeof(InstrumentSnapshot) == 120, "8 + 8 + 2 + 1 + 1 + 4 + 2 x 6 x 8 = 120 B");
static_assert(sizeof(InstrumentSnapshot) == kSnapshotWords * kWordBytes);

// One snapshot plus its version, on its own 128 B line: 8 B version + 120 B snapshot.
// - Why a whole line per instrument: recovery is a cold path, and one line keeps a record's
//   write from touching a neighbour's.
template <class Atomics>
struct alignas(kCacheLineBytes) SnapshotRecord {
  SharedWord<Atomics> version;  // 0 never written, odd while writing, even when complete
  SharedWord<Atomics> snapshot_words[kSnapshotWords];
};
static_assert(sizeof(SnapshotRecord<StdAtomics>) == kCacheLineBytes &&
              offsetof(SnapshotRecord<StdAtomics>, snapshot_words) == kWordBytes);

// What SnapshotTable::read found.
// - Ok: the snapshot holds a complete copy.
// - NeverWritten: version 0, no write ever started for this instrument.
// - GaveUp: the version stayed odd or kept moving for 1 + max_retries attempts. The writer most
//   likely stopped mid-write (a first write interrupted also ends here); the caller keeps the
//   instrument stale.
enum class SnapshotReadStatus : std::uint8_t { Ok, NeverWritten, GaveUp };

// What SnapshotTable::read returns: the status and how many extra attempts it took.
struct SnapshotReadResult {
  SnapshotReadStatus status;
  std::uint32_t retries;  // attempts after the first
};

// The table of SnapshotRecords in a segment, indexed by instrument id.
// - One writer (the bus's Publisher), any number of readers; readers store nothing shared.
template <class Atomics>
class SnapshotTable {
 private:
  SnapshotRecord<Atomics>* records = nullptr;

 public:
  SnapshotTable() = default;
  explicit SnapshotTable(SnapshotRecord<Atomics>* first_record) : records(first_record) {}

  // Replaces the instrument's snapshot: version 4 -> 5 while writing -> 6 when done.
  // - The same store order as RingWriter::publish: odd version, release fence, words, even
  //   version with release.
  // - Only this writer stores the version, so a relaxed load returns its own last (even) store.
  // Example: record 2 at version 4, write(2, snapshot with last_included_seq 41) -> version 6, and
  //   read(2, out) then returns {Ok, retries 0} with out.last_included_seq == 41.
  void write(std::uint16_t instrument_id, const InstrumentSnapshot& snapshot) {
    SnapshotRecord<Atomics>& record = records[instrument_id];
    std::uint64_t words[kSnapshotWords];
    std::memcpy(words, &snapshot, sizeof words);
    const std::uint64_t odd_version = Atomics::load_relaxed(record.version) + 1;
    Atomics::store_relaxed(record.version, odd_version);
    Atomics::fence_release();
    for (std::size_t i = 0; i < kSnapshotWords; ++i)
      Atomics::store_relaxed(record.snapshot_words[i], words[i]);
    Atomics::store_release(record.version, odd_version + 1);
  }

  // Copies the instrument's latest complete snapshot into `snapshot`.
  // - Unlike RingReader::try_poll it retries: a write in progress finishes in nanoseconds, and
  //   any later complete snapshot is as good as the one being replaced.
  // - The cap is there so a writer stopped mid-write cannot hang a reader.
  // - `snapshot` is meaningful only on Ok.
  // Example (default max_retries 256):
  //   version 6 before and after the copy           -> {Ok, retries 0}
  //   version 5 on the first load, 6 on the second  -> {Ok, retries 1}
  //   version 0                                     -> {NeverWritten, retries 0}
  //   version stays 7 (writer killed mid-write)     -> {GaveUp, retries 256}
  SnapshotReadResult read(std::uint16_t instrument_id, InstrumentSnapshot& snapshot,
                          std::uint32_t max_retries = kSnapshotReadMaxRetries) const {
    const SnapshotRecord<Atomics>& record = records[instrument_id];
    std::uint32_t retries = 0;
    while (true) {
      // 1. Version 0: no write ever started (an interrupted first write leaves 1: GaveUp).
      const std::uint64_t version_before_copy = Atomics::load_acquire(record.version);
      if (version_before_copy == 0) return {SnapshotReadStatus::NeverWritten, retries};
      // 2. Even: no write in progress, so copy; the copy counts if the version did not move.
      const bool version_is_even = version_before_copy % 2 == 0;
      if (version_is_even && try_copy(record, version_before_copy, snapshot))
        return {SnapshotReadStatus::Ok, retries};
      // 3. Odd, or a write landed during the copy: give up at the cap, or pause and try again.
      if (retries >= max_retries) return {SnapshotReadStatus::GaveUp, retries};
      spin_pause();
      ++retries;
    }
  }

 private:
  // Copies the record's words into `snapshot` if the version is still version_before_copy after
  // the copy; false leaves `snapshot` untouched.
  static bool try_copy(const SnapshotRecord<Atomics>& record, std::uint64_t version_before_copy,
                       InstrumentSnapshot& snapshot) {
    std::uint64_t words[kSnapshotWords];
    for (std::size_t i = 0; i < kSnapshotWords; ++i) {
      words[i] = Atomics::load_relaxed(record.snapshot_words[i]);
    }
    Atomics::fence_acquire();
    if (Atomics::load_relaxed(record.version) != version_before_copy) return false;
    std::memcpy(&snapshot, words, sizeof snapshot);
    return true;
  }
};

}  // namespace mdbus
