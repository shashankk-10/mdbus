// Runs the real ring and snapshot code, and each mutant from mutants.hpp, on real cores, and
// checks that the stress readers notice every mutant.
// - How: a writer thread publishes at full speed into a 64-slot ring and rewrites two snapshot
//   records, while three reader threads count every torn, stale or out-of-order copy.
// - Each mutant runs with the readers on P cores, then on E cores. Memory ordering between cores
//   is the same whether or not they share a process, so threads are enough.
// - Usage: mutant_stress [ms per placement, default 300]. Exit 0 if the unmodified control saw
//   no bad copy and at least one mutant was killed (made to show one).
// - A failing control means the real protocol tore a copy. A mutant that survives everywhere
//   only says this machine did not expose that reordering in the time given.

#include <pthread/qos.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "mdbus/bus_layout.hpp"
#include "mutants.hpp"
#include "test_helpers.hpp"

using namespace mdbus;
using namespace mdbus::mutants;

namespace {

// 64 slots: small, so the writer laps the readers constantly and every slot is contended.
using StressLayout = BusLayout<DefaultSchema, 64>;
constexpr std::uint16_t kInstrumentCount = 2;

// What each reader thread checks. The value is also its index into StressSharedState::results.
enum class ReaderRole { Follower, Prober, SnapshotChecker };
constexpr int kReaderCount = 3;

// The prober reads slots from one lap behind the head hint up to this many past it.
// - The true head is at most one hint interval past the hint. For 64 slots that interval is 32
//   (RingWriter::kPublishesPerHeadHint is capped at half the ring).
// - So a window of 40 covers slots being overwritten now or within a few publishes.
constexpr std::uint64_t kProbeWindowSlots = 40;

constexpr std::uint64_t kReadyTimeoutNs = 10'000 * kNsPerMillisecond;  // 10 s for threads to start
constexpr unsigned kReadyPollMicroseconds = 200;
// Publishes between the writer's clock reads.
constexpr std::uint64_t kPublishesPerClockCheck = 1024;
// Polls between a reader's loads of the stop flag, so that load stays out of the race too.
constexpr int kPollsPerStopCheck = 256;
// Retries per snapshot read in this test, not the library's 256. GaveUp is allowed and counted;
// a torn copy is a failure.
constexpr std::uint32_t kStressSnapshotReadMaxRetries = 64;

constexpr std::uint64_t kDefaultMsPerPlacement = 300;  // the same as CMake's ctest argument

enum CopyKind { kRingCopy, kSnapshotCopy, kCopyKindCount };  // what a reader checked

// One reader's tally, indexed by CopyKind. copies_checked counts every copy checked, good or bad.
struct CheckCounts {
  std::uint64_t copies_checked[kCopyKindCount];
  std::uint64_t copies_torn_or_stale[kCopyKindCount];
};

// The mutants, one struct each: the printed name and category, and the types it runs.
// - Any part a mutant does not touch is the production type.
// - Categories: control (unmodified), ordering (one weakened atomic operation), structural (a
//   missing step in the reader).

// The production ring and snapshot types, built on the given atomics policy.
template <class MutantAtomics>
struct ProductionTypesOn {
  using Atomics = MutantAtomics;
  using Reader = RingReader<Atomics, StressLayout>;
  using Writer = RingWriter<Atomics, StressLayout>;
  using Snapshots = SnapshotTable<Atomics>;
};

struct Control : ProductionTypesOn<StdAtomics> {
  static constexpr const char* kName = "control_unmodified";
  static constexpr const char* kCategory = "control";
};

struct WriterNoReleaseFence : ProductionTypesOn<NoReleaseFence<StdAtomics>> {
  static constexpr const char* kName = "writer_no_release_fence";
  static constexpr const char* kCategory = "ordering";
};

struct WriterRelaxedDoneStamp : ProductionTypesOn<RelaxedDoneStamp<StdAtomics>> {
  static constexpr const char* kName = "writer_relaxed_done_stamp";
  static constexpr const char* kCategory = "ordering";
};

struct ReaderNoAcquireFence : ProductionTypesOn<NoAcquireFence<StdAtomics>> {
  static constexpr const char* kName = "reader_no_acquire_fence";
  static constexpr const char* kCategory = "ordering";
};

struct ReaderRelaxedFirstStampLoad : ProductionTypesOn<RelaxedFirstStampLoad<StdAtomics>> {
  static constexpr const char* kName = "reader_relaxed_first_stamp_load";
  static constexpr const char* kCategory = "ordering";
};

struct ReaderSkipRecheck : ProductionTypesOn<StdAtomics> {
  static constexpr const char* kName = "reader_skip_recheck";
  static constexpr const char* kCategory = "structural";
  using Reader = SkipRecheckReader<Atomics, StressLayout>;
};

// Everything the writer and readers share: the harness words, then the bus parts under test.
// The harness words are plain std::atomic: they are not under test.
template <class Atomics>
struct StressSharedState {
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> ready;  // readers that have started
  std::atomic<std::uint64_t> go;
  std::atomic<std::uint64_t> stop;
  // The oldest last_included_seq a snapshot read may return, per instrument. The writer raises
  // it right after each snapshot write.
  std::atomic<std::uint64_t> floor[kInstrumentCount];
  CheckCounts results[kReaderCount];
  ControlBlock<Atomics> control;
  Slot<Atomics> ring[StressLayout::kSlotCount];
  SnapshotRecord<Atomics> snapshot_records[kInstrumentCount];
};

// One reader thread: runs its role's check until told to stop, then stores its CheckCounts.
template <class MutantType>
class StressReader {
 private:
  ReaderRole role;
  StressSharedState<typename MutantType::Atomics>* shared;
  const typename MutantType::Reader ring;
  const typename MutantType::Snapshots snapshot_table;
  std::mt19937_64 random_engine;
  CheckCounts result{};
  StressLayout::PayloadWords words{};
  std::uint64_t cursor = 0;
  std::uint64_t last_snapshot_seq[kInstrumentCount] = {};

 public:
  StressReader(ReaderRole reader_role,
               StressSharedState<typename MutantType::Atomics>* shared_memory)
      : role(reader_role),
        shared(shared_memory),
        ring(shared_memory->ring, &shared_memory->control),
        snapshot_table(shared_memory->snapshot_records),
        random_engine(static_cast<std::uint64_t>(reader_role)) {}

  void run() {
    shared->ready.fetch_add(1);
    while (shared->go.load(std::memory_order_acquire) == 0) spin_pause();
    while (shared->stop.load(std::memory_order_acquire) == 0) {
      for (int n = 0; n < kPollsPerStopCheck; ++n) {
        if (role == ReaderRole::Follower)
          follow();
        else if (role == ReaderRole::Prober)
          probe();
        else
          check_snapshot(static_cast<std::uint16_t>(random_engine() % kInstrumentCount));
      }
    }
    shared->results[static_cast<int>(role)] = result;  // read after join, which orders it
  }

 private:
  // Records one checked copy.
  void count(CopyKind kind, bool good) {
    ++result.copies_checked[kind];
    if (!good) ++result.copies_torn_or_stale[kind];
  }

  // Follower: reads every seq in order. Each Ok copy must be whole and be the seq it was polled
  // for; after a lap it jumps forward to the head hint.
  void follow() {
    const TryPollResult poll_result = ring.try_poll(cursor, words);
    if (poll_result == TryPollResult::Ok) {
      count(kRingCopy, is_untorn_pattern(words) && words[0] == cursor);
      ++cursor;
    }
    if (poll_result == TryPollResult::Lapped) cursor = std::max(ring.head_hint(), cursor + 1);
  }

  // Prober: reads a random slot that is about to be overwritten. The copy may be lapped, but an
  // Ok one must be whole. This is where a missing fence shows up first.
  void probe() {
    const std::uint64_t hint = ring.head_hint();
    if (hint < StressLayout::kSlotCount) return;
    const std::uint64_t seq =
        hint - StressLayout::kSlotCount + random_engine() % kProbeWindowSlots;
    if (ring.try_poll(seq, words) == TryPollResult::Ok)
      count(kRingCopy, is_untorn_pattern(words) && words[0] == seq);
  }

  // Snapshot checker: a snapshot must be whole, at least as new as the floor loaded before the
  // read, and never older than the last one accepted.
  // - GaveUp is allowed and not counted. NeverWritten is a failure: every record was prefilled.
  void check_snapshot(std::uint16_t instrument_id) {
    const std::uint64_t floor = shared->floor[instrument_id].load(std::memory_order_acquire);
    InstrumentSnapshot out{};
    const SnapshotReadStatus status =
        snapshot_table.read(instrument_id, out, kStressSnapshotReadMaxRetries).status;
    if (status == SnapshotReadStatus::GaveUp) return;
    const bool good = status == SnapshotReadStatus::Ok && is_untorn_snapshot(out) &&
                      out.last_included_seq >= floor &&
                      out.last_included_seq >= last_snapshot_seq[instrument_id];
    count(kSnapshotCopy, good);
    if (good) last_snapshot_seq[instrument_id] = out.last_included_seq;
  }
};

// The writer: for `ms` milliseconds, publishes pattern message seq and rewrites one snapshot per
// seq, then raises that instrument's floor. Snapshot versions start above the prefill values.
template <class MutantType>
void run_writer(StressSharedState<typename MutantType::Atomics>* shared, std::uint64_t ms) {
  typename MutantType::Writer ring{shared->ring, &shared->control};
  typename MutantType::Snapshots snapshot_table{shared->snapshot_records};
  const std::uint64_t end = steady_clock_ns() + ms * kNsPerMillisecond;

  for (std::uint64_t seq = 0;; ++seq) {
    // The clock only every kPublishesPerClockCheck publishes, so its read stays out of the race.
    if (seq % kPublishesPerClockCheck == 0 && steady_clock_ns() >= end) break;
    const auto instrument_id = static_cast<std::uint16_t>(seq % kInstrumentCount);
    ring.publish(pattern_words<StressLayout::kPayloadWords>(seq));
    snapshot_table.write(instrument_id, pattern_snapshot(seq + kInstrumentCount));
    shared->floor[instrument_id].store(seq + kInstrumentCount, std::memory_order_release);
  }
}

// Asks macOS to run the calling thread on an E core or a P core. It is a QoS hint, not a pin.
void prefer_core_type(bool e_core) {
  qos_class_t qos = QOS_CLASS_USER_INTERACTIVE;
  if (e_core) qos = QOS_CLASS_BACKGROUND;  // BACKGROUND threads run on the E cores
  pthread_set_qos_class_self_np(qos, 0);
}

// Where one run puts its reader threads. `name` is the printed column label.
struct ReaderPlacement {
  const char* name;
  bool e_core;  // where the reader threads run; the writer stays on a P core
};
constexpr ReaderPlacement kReaderPlacements[] = {{"thrP", false}, {"thrE", true}};

// What one reader placement saw.
struct PlacementResult {
  bool readers_started = false;  // false if the readers never became ready
  CheckCounts totals{};          // the three readers' counts, added up
};

// One mutant, one placement:
// 1. Prefill every snapshot record, start the readers and wait for all of them.
// 2. Run the writer for `ms`, then stop and join the readers.
// 3. Add up the readers' counts.
template <class MutantType>
PlacementResult run_one_placement(const ReaderPlacement& placement, std::uint64_t ms) {
  auto shared = std::make_unique<StressSharedState<typename MutantType::Atomics>>();

  typename MutantType::Snapshots snapshot_table(shared->snapshot_records);
  // A complete snapshot in every record first.
  for (std::uint16_t i = 0; i < kInstrumentCount; ++i) {
    snapshot_table.write(i, pattern_snapshot(i));
    shared->floor[i].store(i);
  }

  std::vector<std::thread> threads;
  for (int role = 0; role < kReaderCount; ++role) {
    threads.emplace_back([&shared, role, placement] {
      prefer_core_type(placement.e_core);
      StressReader<MutantType>(static_cast<ReaderRole>(role), shared.get()).run();
    });
  }

  const std::uint64_t deadline = steady_clock_ns() + kReadyTimeoutNs;
  while (shared->ready.load() < kReaderCount && steady_clock_ns() < deadline)
    usleep(kReadyPollMicroseconds);
  PlacementResult result;
  result.readers_started = shared->ready.load() == kReaderCount;

  shared->go.store(1, std::memory_order_release);
  if (result.readers_started) run_writer<MutantType>(shared.get(), ms);
  shared->stop.store(1, std::memory_order_release);
  for (std::thread& thread : threads) thread.join();

  for (const CheckCounts& counts : shared->results) {
    for (int kind = 0; kind < kCopyKindCount; ++kind) {
      result.totals.copies_checked[kind] += counts.copies_checked[kind];
      result.totals.copies_torn_or_stale[kind] += counts.copies_torn_or_stale[kind];
    }
  }
  return result;
}

// The outcome for one mutant over both placements.
struct MutantVerdict {
  bool ran = true;        // every placement's readers started
  bool killed = false;   // some placement saw a bad copy
  bool vacuous = false;  // some placement checked no ring copy or no snapshot copy
};

// Both placements for one mutant, printed as one line: name, category, one column per placement
// (R for a bad ring copy, S for a bad snapshot), then the verdict.
template <class MutantType>
MutantVerdict run_mutant(std::uint64_t ms) {
  std::printf("%-32s %-10s", MutantType::kName, MutantType::kCategory);
  MutantVerdict verdict;
  for (const ReaderPlacement& placement : kReaderPlacements) {
    const PlacementResult result = run_one_placement<MutantType>(placement, ms);
    const CheckCounts& total = result.totals;
    if (!result.readers_started) verdict.ran = false;
    if (total.copies_checked[kRingCopy] == 0 || total.copies_checked[kSnapshotCopy] == 0)
      verdict.vacuous = true;
    if (total.copies_torn_or_stale[kRingCopy] + total.copies_torn_or_stale[kSnapshotCopy] != 0)
      verdict.killed = true;
    const char ring = total.copies_torn_or_stale[kRingCopy] != 0 ? 'R' : '-';
    const char snap = total.copies_torn_or_stale[kSnapshotCopy] != 0 ? 'S' : '-';
    std::printf(" %s:%c%c", placement.name, ring, snap);
  }

  const char* verdict_word = "SURVIVED";
  if (!verdict.ran)
    verdict_word = "ERROR";
  else if (verdict.killed)
    verdict_word = "KILLED";
  else if (verdict.vacuous)
    verdict_word = "INCONCLUSIVE";
  std::printf(" %s\n", verdict_word);
  std::fflush(stdout);
  return verdict;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t ms = kDefaultMsPerPlacement;
  if (argc > 1) ms = std::strtoull(argv[1], nullptr, 10);
  prefer_core_type(false);

  const MutantVerdict control = run_mutant<Control>(ms);
  // In a braced list the calls run in order, so the lines print in this order.
  const MutantVerdict mutants[] = {
      run_mutant<WriterNoReleaseFence>(ms),
      run_mutant<WriterRelaxedDoneStamp>(ms),
      run_mutant<ReaderNoAcquireFence>(ms),
      run_mutant<ReaderRelaxedFirstStampLoad>(ms),
      run_mutant<ReaderSkipRecheck>(ms),
  };

  // The control must run, check something, and see no bad copy; some mutant must be caught.
  const bool control_ok = control.ran && !control.killed && !control.vacuous;
  bool any_killed = false;
  for (const MutantVerdict& verdict : mutants) {
    if (verdict.killed) any_killed = true;
  }
  if (control_ok && any_killed) return 0;
  return 1;
}
