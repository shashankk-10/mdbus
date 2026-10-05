// Failure scenarios with real processes: writers and readers that die, stop or get replaced.
// - How: the writers and readers are this binary re-run as children (test_harness.hpp), then
//   killed with SIGKILL, frozen with SIGSTOP, or replaced by a second writer.
// - A writer that must die mid-write stores exactly what the protocol would have stored by then
//   (the odd stamp, the release fence, some of the words) and raises SIGKILL on itself.
// - Every scenario also runs one deliberate break: it injects the fault its check exists to
//   catch and asserts that the check fires, so no check here can pass vacuously.
// - A failure means a consumer took a half-written message or snapshot as complete, called a
//   live writer dead (or a dead one alive), or a writer's speed depended on its readers.

#include <unistd.h>

#include <cstdio>
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
// Short, so the scenarios reach Down or Stalled quickly (the library default is 100 ms).
constexpr std::uint64_t kTestHeartbeatTimeoutNs = 20 * kNsPerMillisecond;
// The exit status ChildProcess reports for a child killed by a signal is this plus the signal.
constexpr int kExitCodeSignalBase = 128;
// Child exit codes when setup fails, so a CHECK on the exit code tells them apart.
constexpr int kExitOpenFailed = 3;
constexpr int kExitReadyFileFailed = 4;

// Checks every trade against the checksum of the seq its stamp proved, and counts each health
// change it hears about.
struct VerifyingConsumer : Consumer<VerifyingConsumer, SpinWait, TestLayout> {
  std::uint64_t torn = 0;  // trades whose checksum does not match their seq
  std::uint64_t downs = 0;
  std::uint64_t stalls = 0;
  std::uint64_t resumes = 0;  // changes back to Alive
  std::uint64_t reattaches = 0;
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
    if (health == WriterHealth::Stalled) ++stalls;
    if (health == WriterHealth::Alive) ++resumes;
  }

  void on_reattach() {
    ++reattaches;
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
// words written. DoneStampFirst stores the even (complete) version instead of the odd one.
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
//   - stop: SIGSTOPs itself between two publishes, then carries on after SIGCONT.
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
      if (fault == "stop") raise(SIGSTOP);
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

// The file reader child `tag` creates once attached. Readers write no shared memory, so a file
// is how the test learns they are there.
std::string ready_file_path(const std::string& bus, const std::string& tag) {
  BusPaths paths;
  make_bus_paths(bus, paths);
  return paths.lock_directory + "/" + bus + ".ready" + tag;
}

// The reader child: follows the bus until killed, or for 30 s.
int run_following_reader(const std::string& bus, const std::string& tag) {
  constexpr std::uint64_t kFollowMs = 30000;
  VerifyingConsumer consumer;
  if (consumer.attach(bus, kAttachTimeoutNs) != Status::Ok) return kExitOpenFailed;
  std::FILE* ready = std::fopen(ready_file_path(bus, tag).c_str(), "w");
  if (ready == nullptr) return kExitReadyFileFailed;
  std::fclose(ready);
  const std::uint64_t end = steady_clock_ns() + kFollowMs * kNsPerMillisecond;
  while (steady_clock_ns() < end) consumer.poll_once();
  return 0;
}

}  // namespace

CHILD_PROCESS(faulty_writer) {  // argv: bus, fault, fault_seq
  const std::uint64_t fault_seq = std::strtoull(argv[2], nullptr, 10);
  return run_faulty_writer(argv[0], argv[1], fault_seq);
}

CHILD_PROCESS(following_reader) {  // argv: bus, tag
  return run_following_reader(argv[0], argv[1]);
}

// Writers that die, and the writers that replace them.

// The writer is SIGKILLed with seq 1500 half written.
// - The ring has wrapped (1500 - 1024 slots = 476), so the slot's first half still holds seq
//   476's words.
// - The consumer waits on that slot (odd stamp: NotWrittenYet), reports Down once it finds the
//   flock free, and after a new writer replaces the segment it re-attaches and reads on with
//   nothing torn.
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
  CHECK(poll_until(consumer, [&] { return consumer.saw_last_trade; }));
  CHECK(consumer.torn == 0);
  CHECK(consumer.reattaches == 1);
  CHECK(consumer.stats().reattaches == 1);
  CHECK(consumer.resumes >= 1);  // the handler hears Alive again after Down
}

// A half-written slot is never delivered, and a replaced writer is followed onto the new
// segment. Would catch a reader that trusts the stamp without the seq check, or a lost reattach.
TEST(writer_killed_mid_slot_then_replaced) {
  run_slot_fault_case(WriteOrder::Correct);
  run_slot_fault_case(WriteOrder::DoneStampFirst);
}

// The writer is SIGKILLed inside the snapshot write of seq 300.
// - A reader of that instrument (300 % 8 = 4) gets GaveUp after exactly its retry cap.
// - Every other instrument still reads its exact last snapshot.
// - Break: a record stamped complete before it is written is handed out torn as current.
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
    CHECK(read_result.status == SnapshotReadStatus::Ok);
    CHECK(std::memcmp(&out, &want, sizeof out) != 0);
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
// Would catch a snapshot read that skips the version re-check or retries forever.
TEST(writer_killed_mid_snapshot) {
  run_snapshot_fault_case(WriteOrder::Correct);
  run_snapshot_fault_case(WriteOrder::DoneStampFirst);
}

// Writers that stop, and writers that compete.

// The writer is SIGSTOPped between two publishes (seq 700). Its flock is still held, so the
// consumer reports Stalled, never Down, and resumes after SIGCONT.
// - Would catch a monitor that judges by heartbeat age alone and gives up on a paused writer.
// - Break: the same probe without the flock (a null lock path) calls the writer dead.
TEST(writer_stopped_is_stalled_not_down) {
  constexpr std::uint64_t kStopSeq = 700;
  const std::string bus = make_test_bus_name("stop");
  ChildProcess writer(spawn_self("faulty_writer", {bus, "stop", std::to_string(kStopSeq)}));
  VerifyingConsumer consumer;
  REQUIRE(consumer.attach(bus, kAttachTimeoutNs) == Status::Ok);
  consumer.set_heartbeat_timeout(kTestHeartbeatTimeoutNs);
  CHECK(poll_until(consumer, [&] { return consumer.stalls > 0; }));

  WriterHealthMonitor blind;
  blind.start_watching(consumer.pointers().control, nullptr);
  blind.set_heartbeat_timeout(kTestHeartbeatTimeoutNs);
  CHECK(blind.probe_now(steady_clock_ns()) == WriterHealth::Down);

  ::kill(writer.pid, SIGCONT);
  // Past kStopSeq + 1: at least one message published after the SIGCONT has arrived.
  CHECK(poll_until(consumer,
                   [&] { return consumer.resumes > 0 && consumer.next_seq() > kStopSeq + 1; }));
  CHECK(consumer.downs == 0);
  CHECK(consumer.torn == 0);
}

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

// Readers that die.

// Two spinning readers are SIGKILLed without telling anyone. The writer never knows its readers,
// so it laps them at full speed.
// - Would catch any change that makes the writer wait on, or track, a reader.
// - Break: a writer that waits for its slowest reader (backpressure, as a queue would) wedges on
//   the dead one, whose cursor never moves.
// Bus name: dr, for dead readers.
TEST(dead_readers_cost_the_writer_nothing) {
  const std::string bus = make_test_bus_name("dr");
  BusWriter<TestLayout> writer;
  REQUIRE(writer.open(bus, kInstrumentCount) == Status::Ok);
  auto& publisher = writer.publisher();

  ChildProcess first(spawn_self("following_reader", {bus, "0"}));
  ChildProcess second(spawn_self("following_reader", {bus, "1"}));
  CHECK(wait_until([&] {
    publisher.heartbeat_if_due();
    return ::access(ready_file_path(bus, "0").c_str(), F_OK) == 0 &&
           ::access(ready_file_path(bus, "1").c_str(), F_OK) == 0;
  }));

  first.kill_and_wait();
  second.kill_and_wait();
  ::unlink(ready_file_path(bus, "0").c_str());
  ::unlink(ready_file_path(bus, "1").c_str());
  const std::uint64_t dead_cursor = publisher.next_seq();  // where the dead readers stopped

  // WaitForSlowest is a model of a backpressure writer, not mdbus code: it shows what mdbus
  // avoids.
  enum class WriterModel { IgnoreReaders, WaitForSlowest };
  // Publishes four laps; false if they missed the deadline.
  constexpr std::uint64_t kLapCount = 4;
  constexpr std::uint64_t kFourLapsDeadlineMs = 200;  // far more than four laps take on this M1
  const auto four_laps = [&](WriterModel mode) {
    const std::uint64_t end = steady_clock_ns() + kFourLapsDeadlineMs * kNsPerMillisecond;
    for (std::uint64_t i = 0; i < kLapCount * TestLayout::kSlotCount; ++i) {
      if (mode == WriterModel::WaitForSlowest) {
        while (publisher.next_seq() - dead_cursor >= TestLayout::kSlotCount) {
          if (steady_clock_ns() > end) return false;
        }
      }
      publisher.publish(0, Trade{1, 1, 0, {}});
    }
    return steady_clock_ns() <= end;
  };

  CHECK(four_laps(WriterModel::IgnoreReaders));
  CHECK(!four_laps(WriterModel::WaitForSlowest));
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
