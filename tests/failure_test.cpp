// Failure scenarios with real processes: writers that die, compete or get replaced.
// - How: each writer is this binary re-run as a child (test_harness.hpp), then killed with
//   SIGKILL or replaced by a second writer. The readers run in the test.
// - A writer that must die mid-write stores exactly what the protocol would have stored by then
//   (the odd stamp, the release fence, some of the words) and raises SIGKILL on itself.
// - Every scenario also runs one deliberate break: it injects the fault its check exists to
//   catch and asserts that the check fires, so no check here can pass vacuously.
// - A failure means a consumer took a half-written message or snapshot as complete, called a
//   live writer dead (or a dead one alive), or a second writer was let in beside a live one (or
//   kept out once it died).

#include <unistd.h>

#include <cstdlib>
#include <string>

#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;
using namespace mdbus_test;

namespace {

using TestLayout = BusLayout<DefaultSchema, 1024>;
constexpr std::uint32_t kInstrumentCount = 8;
// Every other trade in this test has qty 1, so this one marks the end; the reader stops when it
// sees it.
constexpr std::uint32_t kLastTradeQty = 999'999;
constexpr std::uint64_t kAttachTimeoutNs = 5000 * kNsPerMillisecond;
// Short, so the scenarios reach Down quickly (the library default is 100 ms).
constexpr std::uint64_t kTestHeartbeatTimeoutNs = 20 * kNsPerMillisecond;
// The writer child's exit code when it cannot open its bus, so a CHECK on the exit code tells it
// apart.
constexpr int kExitOpenFailed = 3;

// Checks every trade against the checksum of the seq its stamp proved, and counts each health
// change it hears about.
struct VerifyingConsumer : Consumer<VerifyingConsumer, SpinWait, TestLayout> {
  std::uint64_t torn = 0;  // trades whose checksum does not match their seq
  std::uint64_t downs = 0;
  std::uint64_t resumes = 0;  // changes back to Alive
  bool saw_last_trade = false;

  void on(const Trade& trade, const MessageInfo& info) {
    Payload<TestLayout::kPayloadWords> payload;
    std::memcpy(&payload, info.payload_words, sizeof payload);
    if (payload.header.checksum != payload_checksum(payload, info.seq)) ++torn;
    if (trade.qty == kLastTradeQty) saw_last_trade = true;
  }

  void on(const BookDelta&, const MessageInfo&) {
  }

  void on(const InstrumentStatus&, const MessageInfo&) {
  }

  void on_health_change(WriterHealth health) {
    if (health == WriterHealth::Down) ++downs;
    if (health == WriterHealth::Alive) ++resumes;
  }
};

// Polls the consumer until done() holds; false if that takes over 5 s.
template <class ConsumerType, class Condition>
bool poll_until(ConsumerType& consumer, Condition&& done) {
  constexpr std::uint64_t kPumpMs = 5000;
  const std::uint64_t end = steady_clock_ns() + kPumpMs * kNsPerMillisecond;
  while (!done()) {
    consumer.poll_once();
    if (steady_clock_ns() > end) return false;
  }
  return true;
}

// The order a dying writer stores in: Correct, as the protocol does (the odd stamp or version
// first), or DoneStampFirst, the deliberate break (the stamp or version says complete first).
enum class WriteOrder { Correct, DoneStampFirst };

// Starts writing the slot of `seq` as publish() would, then dies with half the words written.
// - Correct stores the odd stamp; DoneStampFirst stores the even (done) one.
// - Only the second half of the words is written, so the first half, header and checksum
//   included, still holds the previous lap's message. The DoneStampFirst break then shows a mix
//   of two laps under a stamp that says done.
void die_mid_slot(BusWriter<TestLayout>& writer, std::uint64_t seq, WriteOrder order) {
  const Trade trade{static_cast<std::int32_t>(seq), 1, 0, {}};
  const auto words = Publisher<TestLayout>::encode_payload(seq, 0, trade, 0);
  auto& slot = writer.pointers().slots[seq % TestLayout::kSlotCount];

  if (order == WriteOrder::DoneStampFirst)
    StdAtomics::store_relaxed(slot.stamp, stamp_when_written(seq));
  else
    StdAtomics::store_relaxed(slot.stamp, stamp_while_writing(seq));
  StdAtomics::fence_release();
  for (std::size_t i = TestLayout::kPayloadWords / 2; i < TestLayout::kPayloadWords; ++i)
    StdAtomics::store_relaxed(slot.payload_words[i], words[i]);
  raise(SIGKILL);
  std::abort();
}

// Starts instrument_id's snapshot write as SnapshotTable::write would, then dies with half the
// words written.
// - DoneStampFirst stores the even (complete) version instead of the odd one.
// - Only words 0..6 are written: the new seq and bid. The ask keeps the last complete snapshot's,
//   so the record matches neither snapshot (see make_test_snapshot).
void die_mid_snapshot(BusWriter<TestLayout>& writer, std::uint16_t instrument_id,
                      std::uint64_t seq, WriteOrder order) {
  const InstrumentSnapshot snapshot = make_test_snapshot(instrument_id, seq);
  std::uint64_t words[kSnapshotWords];
  std::memcpy(words, &snapshot, sizeof words);
  SnapshotRecord<StdAtomics>& record = writer.pointers().snapshot_records[instrument_id];
  const std::uint64_t odd_version = StdAtomics::load_relaxed(record.version) + 1;

  if (order == WriteOrder::DoneStampFirst)
    StdAtomics::store_relaxed(record.version, odd_version + 1);
  else
    StdAtomics::store_relaxed(record.version, odd_version);
  StdAtomics::fence_release();
  for (std::size_t i = 0; i < kSnapshotWords / 2; ++i)
    StdAtomics::store_relaxed(record.snapshot_words[i], words[i]);
  raise(SIGKILL);
  std::abort();
}

// The writer child. argv: bus, fault, fault_seq.
// - Publishes one trade per seq plus that instrument's snapshot, pausing often enough that
//   readers are not lapped. A late reader starts at the head hint, which is never past
//   fault_seq, so it still meets the fault.
// - At fault_seq it does what `fault` says:
//   - none: publishes the last trade and idles until killed.
//   - slot / slot_naive: dies mid-slot (Correct / DoneStampFirst).
//   - snap / snap_naive: publishes the trade, then dies mid-snapshot (Correct / DoneStampFirst).
int run_faulty_writer(const std::string& bus, const std::string& fault, std::uint64_t fault_seq) {
  constexpr std::uint64_t kPublishesPerPause = 64;
  constexpr unsigned kPauseMicroseconds = 100;
  constexpr int kIdleMs = 30000;  // how long the writer stays alive after fault_seq

  BusWriter<TestLayout> writer;
  if (writer.open(bus, kInstrumentCount) != Status::Ok) return kExitOpenFailed;

  WriteOrder order = WriteOrder::Correct;
  if (fault == "slot_naive" || fault == "snap_naive") order = WriteOrder::DoneStampFirst;

  auto& publisher = writer.publisher();
  while (true) {
    const std::uint64_t seq = publisher.next_seq();
    const auto instrument_id = static_cast<std::uint16_t>(seq % kInstrumentCount);
    const Trade trade{static_cast<std::int32_t>(seq), 1, 0, {}};
    if (seq == fault_seq) {
      if (fault == "none") break;
      if (fault == "slot" || fault == "slot_naive") die_mid_slot(writer, seq, order);
      if (fault == "snap" || fault == "snap_naive") {
        publisher.publish(instrument_id, trade);
        die_mid_snapshot(writer, instrument_id, seq, order);
      }
    }
    publisher.publish_and_update_snapshot(instrument_id, trade,
                                          make_test_snapshot(instrument_id, seq));
    if (seq % kPublishesPerPause == kPublishesPerPause - 1) {
      publisher.heartbeat_if_due();
      usleep(kPauseMicroseconds);
    }
  }

  publisher.publish(0, Trade{0, kLastTradeQty, 0, {}});
  for (int i = 0; i < kIdleMs; ++i) {  // stay alive until killed
    publisher.heartbeat_if_due();
    sleep_ms(1);
  }
  return 0;
}

// Attaches to the bus's live writer. A replacement makes a fresh segment, and until it does the
// name still leads to the dead writer's, whose heartbeat has stopped: what mdbus_watch does each
// second while its writer reads Down.
bool attach_to_live_writer(VerifyingConsumer& consumer, const std::string& bus) {
  constexpr std::uint64_t kWaitMs = 5000;
  const std::uint64_t end = steady_clock_ns() + kWaitMs * kNsPerMillisecond;
  while (steady_clock_ns() < end) {
    BusReader<TestLayout> candidate;
    if (candidate.open(bus) == Status::Ok &&
        judge_writer(*candidate.pointers().control, steady_clock_ns(), UINT64_MAX,
                     kTestHeartbeatTimeoutNs) == WriterHealth::Alive)
      return consumer.attach(bus) == Status::Ok;
    sleep_ms(1);
  }
  return false;
}

}  // namespace

CHILD_PROCESS(faulty_writer) {  // argv: bus, fault, fault_seq
  const std::uint64_t fault_seq = std::strtoull(argv[2], nullptr, 10);
  return run_faulty_writer(argv[0], argv[1], fault_seq);
}

// Writers that die, and the writers that replace them.

// The writer is SIGKILLed with seq 1500 half written.
// - The ring has wrapped (1500 - 1024 slots = 476), so the slot's first half still holds seq
//   476's words.
// - The consumer waits on that slot (odd stamp: NotWrittenYet) and reports Down once the
//   heartbeat is older than the timeout. A new writer makes a fresh segment; the consumer
//   attaches to it and reads on with nothing torn.
// - Break: a writer that marks the slot done before writing it exposes a mix of two laps, which
//   the checksum must catch.
// Bus names: p1 for the correct case, p1n for the break ("n" for naive).
static void run_slot_fault_case(WriteOrder order) {
  constexpr std::uint64_t kFaultSeq = 1500;
  const bool is_break = order == WriteOrder::DoneStampFirst;
  const std::string bus = make_test_bus_name(is_break ? "p1n" : "p1");
  const std::string fault = is_break ? "slot_naive" : "slot";
  ChildProcess dead(spawn_self("faulty_writer", {bus, fault, std::to_string(kFaultSeq)}));
  VerifyingConsumer consumer;
  REQUIRE(consumer.attach(bus, kAttachTimeoutNs) == Status::Ok);
  consumer.set_heartbeat_timeout(kTestHeartbeatTimeoutNs);

  if (is_break) {
    CHECK(poll_until(consumer, [&] { return consumer.torn > 0; }));
    return;
  }
  CHECK(poll_until(consumer, [&] { return consumer.downs > 0; }));
  const int exit_code = dead.wait_for_exit_code();
  CHECK(consumer.next_seq() == kFaultSeq);
  CHECK(exit_code == kExitCodeSignalBase + SIGKILL);
  CHECK(consumer.torn == 0);

  // The replacement publishes up to seq 2000, then the last trade.
  ChildProcess next(spawn_self("faulty_writer", {bus, "none", "2000"}));
  REQUIRE(attach_to_live_writer(consumer, bus));
  CHECK(poll_until(consumer, [&] { return consumer.saw_last_trade; }));
  CHECK(consumer.torn == 0);
  CHECK(consumer.resumes >= 1);  // the handler hears Alive again after Down
}

// A half-written slot is never delivered, and the replacement's segment reads on cleanly.
// Would catch a reader that trusts the stamp without the seq check.
TEST(writer_killed_mid_slot_then_replaced) {
  run_slot_fault_case(WriteOrder::Correct);
  run_slot_fault_case(WriteOrder::DoneStampFirst);
}

// The writer is SIGKILLed inside the snapshot write of seq 300.
// - A reader of that instrument (300 % 8 = 4) gets GaveUp after exactly its retry cap.
// - Every other instrument still reads its exact last snapshot.
// - Break: a record stamped complete before it is written is handed out as current, though it
//   is torn: seq 300's bid with seq 292's ask, neither snapshot.
// Bus names: p2 for the correct case, p2n for the break.
static void run_snapshot_fault_case(WriteOrder order) {
  constexpr std::uint64_t kFaultSeq = 300;
  constexpr auto kFaultInstrumentId = static_cast<std::uint16_t>(kFaultSeq % kInstrumentCount);
  const bool is_break = order == WriteOrder::DoneStampFirst;
  const std::string bus = make_test_bus_name(is_break ? "p2n" : "p2");
  const std::string fault = is_break ? "snap_naive" : "snap";
  ChildProcess dead(spawn_self("faulty_writer", {bus, fault, std::to_string(kFaultSeq)}));
  BusReader<TestLayout> reader;
  REQUIRE(reader.open(bus, kAttachTimeoutNs) == Status::Ok);
  const int exit_code = dead.wait_for_exit_code();
  REQUIRE(exit_code == kExitCodeSignalBase + SIGKILL);

  const SnapshotTable<StdAtomics> snapshot_table(reader.pointers().snapshot_records);
  InstrumentSnapshot out{};
  // The faulted instrument's last complete snapshot, one round of instruments earlier (seq 292).
  InstrumentSnapshot want = make_test_snapshot(kFaultInstrumentId, kFaultSeq - kInstrumentCount);
  const SnapshotReadResult read_result =
      snapshot_table.read(kFaultInstrumentId, out, kSnapshotReadMaxRetries);
  if (is_break) {  // a torn snapshot handed out as current
    const InstrumentSnapshot intended = make_test_snapshot(kFaultInstrumentId, kFaultSeq);
    CHECK(read_result.status == SnapshotReadStatus::Ok);
    CHECK(std::memcmp(&out, &want, sizeof out) != 0);
    CHECK(std::memcmp(&out, &intended, sizeof out) != 0);
  } else {  // the reader waits out its cap rather than take the half-written snapshot
    CHECK(read_result.status == SnapshotReadStatus::GaveUp);
    CHECK(read_result.retries == kSnapshotReadMaxRetries);
  }

  for (std::uint16_t i = 0; i < kInstrumentCount; ++i) {
    if (i == kFaultInstrumentId) continue;
    // Instrument i was last published at the newest seq <= kFaultSeq with
    // seq % kInstrumentCount == i.
    const std::uint64_t last_seq = kFaultSeq - (kFaultSeq - i) % kInstrumentCount;
    want = make_test_snapshot(i, last_seq);
    const SnapshotReadStatus status = snapshot_table.read(i, out).status;
    CHECK(status == SnapshotReadStatus::Ok);
    CHECK(std::memcmp(&out, &want, sizeof out) == 0);
  }
}

// A half-written snapshot is never handed out, and a dead writer's other records stay readable.
// Would catch a snapshot read that takes an odd (mid-write) version as complete, or retries
// forever.
TEST(writer_killed_mid_snapshot) {
  run_snapshot_fault_case(WriteOrder::Correct);
  run_snapshot_fault_case(WriteOrder::DoneStampFirst);
}

// Writers that compete.

// A second writer is refused while the first holds the flock, stopped or not, and takes over
// once the first is dead.
// - Would catch a lock that is checked by pid or heartbeat instead of held by the kernel.
// - Break: unlinking the lock file under a live writer (what a tmp cleaner does) lets a newcomer
//   lock a fresh inode. That is why the lock lives in a private 0700 directory in /var/tmp.
TEST(second_writer_refused) {
  const std::string bus = make_test_bus_name("two");
  // A fault seq the test never reaches: the first writer just keeps publishing.
  ChildProcess first(spawn_self("faulty_writer", {bus, "none", "1000000000"}));
  BusReader<TestLayout> reader;
  REQUIRE(reader.open(bus, kAttachTimeoutNs) == Status::Ok);

  BusWriter<TestLayout> second;
  BusWriter<TestLayout> third;
  CHECK(second.open(bus, kInstrumentCount) == Status::AnotherWriterRunning);
  ::kill(first.pid, SIGSTOP);
  CHECK(second.open(bus, kInstrumentCount) == Status::AnotherWriterRunning);
  first.kill_and_wait();
  CHECK(second.open(bus, kInstrumentCount) == Status::Ok);

  BusPaths paths;
  const bool paths_made = make_bus_paths(bus, paths);
  REQUIRE(paths_made);
  struct stat info{};
  const int stat_result = stat(paths.lock_directory.c_str(), &info);
  REQUIRE(stat_result == 0);
  CHECK((info.st_mode & 0777) == 0700);  // owner-only permission bits
  REQUIRE(::unlink(paths.lock_file.c_str()) == 0);
  CHECK(third.open(bus, kInstrumentCount) == Status::Ok);
}
