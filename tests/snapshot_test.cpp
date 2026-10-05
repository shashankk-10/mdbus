// The snapshot table's seqlock on one thread.
// - Reads return NeverWritten before the first write, then exactly the latest snapshot.
// - A write that lands during the copy makes the reader retry, and the retry returns the newer
//   snapshot whole.
// - A writer stopped inside a write (simulated by leaving the version odd) makes the reader retry
//   up to its cap and report GaveUp, never hand out the half-written record.
// - A failure means a recovering consumer could start from a torn or stale book.

#include "mdbus/segment_format.hpp"
#include "mdbus/snapshot_table.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

namespace {
using TestLayout = BusLayout<DefaultSchema, 128>;  // 128 slots; the ring is not used here
constexpr std::uint32_t kInstrumentCount = 4;

bool snapshots_equal(const InstrumentSnapshot& left, const InstrumentSnapshot& right) {
  return std::memcmp(&left, &right, sizeof left) == 0;
}
}  // namespace

// Five writes to one record, each read back exactly; a neighbour record stays NeverWritten.
// Would catch a read that returns an older version or bleeds into the next record.
TEST(seqlock_basic) {
  constexpr std::uint64_t kWriteCount = 5;
  InProcessBus<TestLayout> bus(kInstrumentCount);
  SnapshotTable<StdAtomics> snapshot_table(bus.pointers().snapshot_records);
  InstrumentSnapshot out;
  CHECK(snapshot_table.read(1, out).status == SnapshotReadStatus::NeverWritten);
  for (std::uint64_t seq = 1; seq <= kWriteCount; ++seq) {
    snapshot_table.write(1, make_test_snapshot(1, seq));
    REQUIRE(snapshot_table.read(1, out).status == SnapshotReadStatus::Ok);
    CHECK(snapshots_equal(out, make_test_snapshot(1, seq)));
  }
  CHECK(snapshot_table.read(2, out).status == SnapshotReadStatus::NeverWritten);
}

// A write lands while the reader copies version 2: the re-check sees version 4, so the copy is
// thrown away and one retry returns the new snapshot whole.
// Would catch a read that keeps the copy it took while a write was landing.
TEST(write_during_copy_is_retried) {
  SnapshotRecord<WriteDuringCopy> records[kInstrumentCount]{};
  SnapshotTable<WriteDuringCopy> snapshot_table(records);
  snapshot_table.write(1, make_test_snapshot(1, 7));
  WriteDuringCopy::hook = [&] { snapshot_table.write(1, make_test_snapshot(1, 8)); };
  InstrumentSnapshot out;
  const SnapshotReadResult read_result = snapshot_table.read(1, out);
  CHECK(read_result.status == SnapshotReadStatus::Ok && read_result.retries == 1);
  CHECK(snapshots_equal(out, make_test_snapshot(1, 8)));
  CHECK(!WriteDuringCopy::hook);  // it ran
}

// An odd version that never turns even: the read gives up after exactly kRetryCap retries.
// Would catch a reader that spins forever on a dead writer, or accepts an odd version.
TEST(seqlock_writer_stopped_mid_update) {
  constexpr std::uint32_t kRetryCap = 8;  // small, so the test does not wait out the default 256
  InProcessBus<TestLayout> bus(kInstrumentCount);
  SnapshotTable<StdAtomics> snapshot_table(bus.pointers().snapshot_records);
  snapshot_table.write(0, make_test_snapshot(0, 7));
  auto& record = bus.pointers().snapshot_records[0];
  // Odd: the writer is mid-write.
  StdAtomics::store_relaxed(record.version, StdAtomics::load_relaxed(record.version) + 1);
  InstrumentSnapshot out;
  const SnapshotReadResult read_result = snapshot_table.read(0, out, kRetryCap);
  CHECK(read_result.status == SnapshotReadStatus::GaveUp);
  CHECK(read_result.retries == kRetryCap);
}
