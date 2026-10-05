// Runs the real ring and snapshot code, and each mutant from mutants.hpp, on real cores, and
// checks that the stress readers catch every mutant.
// - How: a writer thread publishes at full speed into a 64-slot ring and rewrites two snapshot
//   records, while three reader threads count every torn, stale or out-of-order copy.
// - A round runs four cells: two stress layouts x two reader placements (P cores, E cores).
//   Memory ordering between cores is the same whether or not they share a process, so threads
//   are enough.
// - Usage: mutant_stress [MS [--with-equivalent-mutant]]: MS, the ms per cell (default 300),
//   comes first, and text that is not a number reads as 0. Exit 0 only if the unmodified control
//   saw no bad copy in any cell and every mutant was killed (made to show one in some cell).
// - A mutant that survives a round gets up to kMaxReruns more before it counts as SURVIVED. Kills
//   are random, and the writer-side mutants' come almost only from the P-core cells, so on a
//   loaded machine one quiet round proves little.
// - --with-equivalent-mutant adds a mutant that is the production code, which nothing can kill,
//   so the run must fail. ctest's mutant_stress_refuses_a_survivor checks the exit rule that way.
// - A failing control means the real protocol tore a copy. A mutant that survives every round
//   only says this machine did not expose that reordering in the time given.

#include <pthread/qos.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mdbus/segment_format.hpp"
#include "mutants.hpp"
#include "test_helpers.hpp"

using namespace mdbus;
using namespace mdbus::mutants;

namespace {

// TSan turns every atomic access and fence into a call into its runtime, which hides the
// reorderings the ordering mutants rely on: all four survive under it (reader_skip_recheck
// still dies). So a TSan build runs the control alone, and the mutant verdicts come from the
// default and ASan builds.
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif

// The two stress layouts. 64 slots: small, so the writer laps the readers constantly and every
// slot is contended. Both keep the production 128 B slot.
// - SevenWordLayout: the production payload, so stamp + payload sit in the first 64 B half.
// - FifteenWordLayout: the Copy15 shape, stamp + payload across both 64 B halves.
// - Why both: on the M1 Pro measured below, the writer's stores inside one 64 B half were never
//   seen out of order, so in the 7-word ring the writer-side mutants tore no copy, and were
//   caught only through the snapshot record (8 B version + 120 B, across both halves). In the
//   15-word ring they tear ring copies too, so the ring writer's fence and release store are
//   under test.
// - Measured on an M1 Pro, 10 runs: under the two writer-side mutants the 7-word ring tore 0 of
//   about 330 M copies and the 15-word ring about 7,100 of 240 M.
using SevenWordLayout = BusLayout<DefaultSchema, 64>;
using FifteenWordLayout = BusLayout<DefaultSchema, 64, 15, kCacheLineBytes>;
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

constexpr std::uint64_t kDefaultMsPerCell = 300;  // the same as CMake's ctest argument
// A mutant with no bad copy in a round is run again, up to this many more rounds, each at
// kRerunTimeFactor times the first round's time per cell.
constexpr int kMaxReruns = 2;
constexpr std::uint64_t kRerunTimeFactor = 2;

enum CopyKind { kRingCopy, kSnapshotCopy, kCopyKindCount };  // what a reader checked

// One reader's tally, or the sum of several, indexed by CopyKind. copies_checked counts every
// copy checked, good or bad.
struct CheckCounts {
  std::uint64_t copies_checked[kCopyKindCount];
  std::uint64_t copies_torn_or_stale[kCopyKindCount];
  std::uint64_t snapshot_reads_gave_up;  // GaveUp hands out no copy to check

  void add(const CheckCounts& other) {
    for (int kind = 0; kind < kCopyKindCount; ++kind) {
      copies_checked[kind] += other.copies_checked[kind];
      copies_torn_or_stale[kind] += other.copies_torn_or_stale[kind];
    }
    snapshot_reads_gave_up += other.snapshot_reads_gave_up;
  }
};

// The types one mutant runs: the production ring and snapshot code on an atomics policy, with the
// ring reader swapped for a broken one (SkipRecheckReader) where a mutant needs it. The ring types
// take the stress layout.
template <class MutantAtomics, template <class, class> class RingReaderType = RingReader>
struct MutantTypes {
  using Atomics = MutantAtomics;
  template <class Layout>
  using Reader = RingReaderType<Atomics, Layout>;
  template <class Layout>
  using Writer = RingWriter<Atomics, Layout>;
  using Snapshots = SnapshotTable<Atomics>;
};

// Everything the writer and readers share: the harness words, then the bus parts under test.
// The harness words are plain std::atomic: they are not under test.
template <class Atomics, class Layout>
struct StressSharedState {
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> ready;  // readers that have started
  std::atomic<std::uint64_t> go;
  std::atomic<std::uint64_t> stop;
  // The oldest last_included_seq a snapshot read may return, per instrument. The writer raises
  // it right after each snapshot write.
  std::atomic<std::uint64_t> floor[kInstrumentCount];
  CheckCounts results[kReaderCount];
  ControlBlock<Atomics> control;
  Slot<Atomics, Layout::kPayloadWords, Layout::kSlotBytes> ring[Layout::kSlotCount];
  SnapshotRecord<Atomics> snapshot_records[kInstrumentCount];
};

// One reader thread: runs its role's check until told to stop, then stores its CheckCounts.
template <class Mutant, class Layout>
class StressReader {
 private:
  ReaderRole role;
  StressSharedState<typename Mutant::Atomics, Layout>* shared;
  const typename Mutant::template Reader<Layout> ring;
  const typename Mutant::Snapshots snapshot_table;
  std::mt19937_64 random_engine;
  CheckCounts result{};
  typename Layout::PayloadWords words{};
  std::uint64_t cursor = 0;
  std::uint64_t last_snapshot_seq[kInstrumentCount] = {};

 public:
  StressReader(ReaderRole reader_role,
               StressSharedState<typename Mutant::Atomics, Layout>* shared_memory)
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
    if (hint < Layout::kSlotCount) return;
    const std::uint64_t seq = hint - Layout::kSlotCount + random_engine() % kProbeWindowSlots;
    if (ring.try_poll(seq, words) == TryPollResult::Ok)
      count(kRingCopy, is_untorn_pattern(words) && words[0] == seq);
  }

  // Snapshot checker: a snapshot must be whole, at least as new as the floor loaded before the
  // read, and never older than the last one accepted.
  // - The read is Consumer's call, with the library's retry cap (kSnapshotReadMaxRetries).
  // - GaveUp is allowed and counted. NeverWritten is a failure: every record was prefilled.
  void check_snapshot(std::uint16_t instrument_id) {
    const std::uint64_t floor = shared->floor[instrument_id].load(std::memory_order_acquire);
    InstrumentSnapshot out{};
    const SnapshotReadStatus status = snapshot_table.read(instrument_id, out).status;
    if (status == SnapshotReadStatus::GaveUp) {
      ++result.snapshot_reads_gave_up;
      return;
    }
    const bool good = status == SnapshotReadStatus::Ok && is_untorn_snapshot(out) &&
                      out.last_included_seq >= floor &&
                      out.last_included_seq >= last_snapshot_seq[instrument_id];
    count(kSnapshotCopy, good);
    if (good) last_snapshot_seq[instrument_id] = out.last_included_seq;
  }
};

// The writer: for `ms` milliseconds, publishes pattern message seq and rewrites one snapshot per
// seq, then raises that instrument's floor. Snapshot versions start above the prefill values.
template <class Mutant, class Layout>
void run_writer(StressSharedState<typename Mutant::Atomics, Layout>* shared, std::uint64_t ms) {
  typename Mutant::template Writer<Layout> ring{shared->ring, &shared->control};
  typename Mutant::Snapshots snapshot_table{shared->snapshot_records};
  const std::uint64_t end = steady_clock_ns() + ms * kNsPerMillisecond;

  for (std::uint64_t seq = 0;; ++seq) {
    // The clock only every kPublishesPerClockCheck publishes, so its read stays out of the race.
    if (seq % kPublishesPerClockCheck == 0 && steady_clock_ns() >= end) break;
    const auto instrument_id = static_cast<std::uint16_t>(seq % kInstrumentCount);
    ring.publish(pattern_words<Layout::kPayloadWords>(seq));
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

// Where one cell puts its reader threads. `name` is the printed label.
struct ReaderPlacement {
  const char* name;
  bool e_core;  // where the reader threads run; the writer stays on a P core
};
constexpr ReaderPlacement kReaderPlacements[] = {{"P", false}, {"E", true}};
constexpr std::size_t kCellCount = 2 * std::size(kReaderPlacements);  // x the two layouts

// What one cell saw.
struct CellResult {
  bool readers_started = false;  // false if the readers never became ready
  CheckCounts totals{};          // the three readers' counts, added up
};

// One mutant in one cell:
// 1. Prefill every snapshot record, start the readers and wait for all of them.
// 2. Run the writer for `ms`, then stop and join the readers.
// 3. Add up the readers' counts.
template <class Mutant, class Layout>
CellResult run_cell(const ReaderPlacement& placement, std::uint64_t ms) {
  auto shared = std::make_unique<StressSharedState<typename Mutant::Atomics, Layout>>();

  typename Mutant::Snapshots snapshot_table(shared->snapshot_records);
  // A complete snapshot in every record first.
  for (std::uint16_t i = 0; i < kInstrumentCount; ++i) {
    snapshot_table.write(i, pattern_snapshot(i));
    shared->floor[i].store(i);
  }

  std::vector<std::thread> threads;
  for (int role = 0; role < kReaderCount; ++role) {
    threads.emplace_back([&shared, role, placement] {
      prefer_core_type(placement.e_core);
      StressReader<Mutant, Layout>(static_cast<ReaderRole>(role), shared.get()).run();
    });
  }

  const std::uint64_t deadline = steady_clock_ns() + kReadyTimeoutNs;
  while (shared->ready.load() < kReaderCount && steady_clock_ns() < deadline)
    usleep(kReadyPollMicroseconds);
  CellResult result;
  result.readers_started = shared->ready.load() == kReaderCount;

  shared->go.store(1, std::memory_order_release);
  if (result.readers_started) run_writer<Mutant, Layout>(shared.get(), ms);
  shared->stop.store(1, std::memory_order_release);
  for (std::thread& thread : threads) thread.join();

  for (const CheckCounts& counts : shared->results)
    result.totals.add(counts);
  return result;
}

// One round: every cell once, in table order.
struct Round {
  CheckCounts cells[kCellCount]{};
  std::size_t cells_run = 0;
  bool ran = true;       // every cell's readers started
  bool killed = false;   // some cell saw a bad copy
  bool vacuous = false;  // some cell checked no ring copy or no snapshot copy

  void add_cell(const CellResult& result) {
    const CheckCounts& counts = result.totals;
    if (!result.readers_started) ran = false;
    if (counts.copies_checked[kRingCopy] == 0 || counts.copies_checked[kSnapshotCopy] == 0)
      vacuous = true;
    if (counts.copies_torn_or_stale[kRingCopy] + counts.copies_torn_or_stale[kSnapshotCopy] != 0)
      killed = true;
    cells[cells_run++] = counts;
  }
};

template <class Mutant>
Round run_round(std::uint64_t ms) {
  Round round;
  for (const ReaderPlacement& placement : kReaderPlacements)
    round.add_cell(run_cell<Mutant, SevenWordLayout>(placement, ms));
  for (const ReaderPlacement& placement : kReaderPlacements)
    round.add_cell(run_cell<Mutant, FifteenWordLayout>(placement, ms));
  return round;
}

// Column widths of the table.
constexpr int kLabelWidth = 30;
constexpr int kCellWidth = 11;

template <class Layout>
void print_cell_labels() {
  for (const ReaderPlacement& placement : kReaderPlacements) {
    const std::string label = std::to_string(Layout::kPayloadWords) + "w/" + placement.name;
    std::printf(" %-*s", kCellWidth, label.c_str());
  }
}

void print_legend_and_header(std::uint64_t ms) {
  std::printf(
      "mutant_stress: a writer and 3 reader threads, %s ms per cell. 7w is the production\n"
      "slot (stamp + payload in one 64 B half), 15w the Copy15 shape (across both halves); P and\n"
      "E put the readers on P or E cores. A cell shows bad ring / bad snapshot copies, and one\n"
      "kills a mutant. Checked: millions of copies (ring / snapshot). Gave up: snapshot reads\n"
      "that hit the retry cap. On the M1 Pro this was written on, the 7w ring never showed a\n"
      "writer-side reordering (0 of about 330 M copies in 10 runs), so those mutants died there\n"
      "only through the snapshot record; 15w lets the ring check catch them too.\n\n",
      std::to_string(ms).c_str());
  std::printf("%-*s", kLabelWidth, "mutant");
  print_cell_labels<SevenWordLayout>();
  print_cell_labels<FifteenWordLayout>();
  std::printf(" %-13s %-8s %s\n", "checked", "gave up", "verdict");
}

// One table row: the label, each cell's bad ring / bad snapshot copies, the copies checked in all
// cells, the snapshot reads that gave up, and the verdict.
void print_row(const char* label, const Round& round, const char* verdict) {
  CheckCounts sum{};
  std::printf("%-*s", kLabelWidth, label);
  for (const CheckCounts& cell : round.cells) {
    const std::string bad = std::to_string(cell.copies_torn_or_stale[kRingCopy]) + "/" +
                            std::to_string(cell.copies_torn_or_stale[kSnapshotCopy]);
    std::printf(" %-*s", kCellWidth, bad.c_str());
    sum.add(cell);
  }
  constexpr double kMillion = 1e6;
  char checked[32];
  std::snprintf(checked, sizeof checked, "%.1f/%.1f",
                static_cast<double>(sum.copies_checked[kRingCopy]) / kMillion,
                static_cast<double>(sum.copies_checked[kSnapshotCopy]) / kMillion);
  std::printf(" %-13s %-8s %s\n", checked, std::to_string(sum.snapshot_reads_gave_up).c_str(),
              verdict);
  std::fflush(stdout);
}

// The word printed for a round. The control must be clean; a mutant must be killed, and a
// survivor's last round says SURVIVED.
const char* verdict_word(const Round& round, bool is_control, bool last_round) {
  if (!round.ran) return "ERROR";  // some cell's readers never started
  if (is_control) {
    if (round.killed) return "FAILED";
    return round.vacuous ? "INCONCLUSIVE" : "clean";
  }
  if (round.killed) return "KILLED";
  if (!last_round) return "rerun";
  return round.vacuous ? "INCONCLUSIVE" : "SURVIVED";
}

// Runs a mutant's rounds (reruns as kMaxReruns says; the control runs once), one table row each,
// and returns the last.
template <class Mutant>
Round run_mutant(const char* name, std::uint64_t ms, bool is_control = false) {
  const int reruns = is_control ? 0 : kMaxReruns;
  Round round = run_round<Mutant>(ms);
  print_row(name, round, verdict_word(round, is_control, reruns == 0));
  for (int rerun = 1; rerun <= reruns && !round.killed; ++rerun) {
    const std::uint64_t rerun_ms = kRerunTimeFactor * ms;
    round = run_round<Mutant>(rerun_ms);
    const std::string label = "  rerun at " + std::to_string(rerun_ms) + " ms";
    print_row(label.c_str(), round, verdict_word(round, is_control, rerun == reruns));
  }
  return round;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t ms = kDefaultMsPerCell;
  if (argc > 1) ms = std::strtoull(argv[1], nullptr, 10);
  const bool with_equivalent_mutant =
      argc > 2 && std::string_view(argv[2]) == "--with-equivalent-mutant";
  prefer_core_type(false);
  print_legend_and_header(ms);

  const Round control = run_mutant<MutantTypes<StdAtomics>>("control_unmodified", ms, true);
  // The control must run, check something in every cell, and see no bad copy.
  const bool control_clean = control.ran && !control.killed && !control.vacuous;
  if (kThreadSanitizer) {
    std::printf(
        "\nTSan build: the mutants are skipped. TSan's instrumentation hides the reorderings the\n"
        "ordering mutants rely on (all four survive under it). Control %s.\n",
        control_clean ? "clean" : "NOT clean");
    return control_clean ? 0 : 1;
  }

  // Four ordering mutants (one atomic-policy operation weakened at every call site), then one
  // structural mutant. In a braced list the calls run in order, so the rows print in this order.
  std::vector<Round> mutants = {
      run_mutant<MutantTypes<NoReleaseFence<StdAtomics>>>("writer_no_release_fence", ms),
      run_mutant<MutantTypes<ReleaseStoresRelaxed<StdAtomics>>>("writer_release_stores_relaxed",
                                                                ms),
      run_mutant<MutantTypes<NoAcquireFence<StdAtomics>>>("reader_no_acquire_fence", ms),
      run_mutant<MutantTypes<AcquireLoadsRelaxed<StdAtomics>>>("reader_acquire_loads_relaxed", ms),
      run_mutant<MutantTypes<StdAtomics, SkipRecheckReader>>("reader_skip_recheck", ms),
  };
  // The production code under a mutant's name, which nothing can kill.
  if (with_equivalent_mutant)
    mutants.push_back(run_mutant<MutantTypes<StdAtomics>>("equivalent_mutant", ms));
  std::size_t killed = 0;
  for (const Round& round : mutants) {
    if (round.killed) ++killed;
  }
  const int exit_code = control_clean && killed == mutants.size() ? 0 : 1;
  std::printf("\nControl %s, %zu of %zu mutants killed: exit %d.\n",
              control_clean ? "clean" : "NOT clean", killed, mutants.size(), exit_code);
  return exit_code;
}
