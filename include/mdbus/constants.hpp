#pragma once

// Compile-time constants of the bus, in two groups:
// - Shared-memory shape: fed into BusLayout::layout_hash(), so a reader built with different
//   values refuses to attach (Status::LayoutMismatch).
// - Defaults and timings: NOT in the hash. A writer and a reader built with different values
//   still attach; keep them equal by building both from one tree.
// - Also holds the arm64-only guard. clock.hpp includes this file for the guard alone.

#include <cstddef>
#include <cstdint>

// The tick clock (cntvct_el0), the isb pauses and the 128 B line size are all M1 facts.
#if !defined(__aarch64__) || !defined(__APPLE__)
#error "mdbus targets Apple Silicon (arm64 macOS) only"
#endif

namespace mdbus {

// Shared-memory shape (fed into the layout hash).

// 128 B: the cache-line size macOS reports on M1 (sysctl hw.cachelinesize).
// - Every shared struct that a different core writes gets its own 128 B line, so two of them
//   never share a line (false sharing: two cores writing different data in one line).
// - Why not std::hardware_destructive_interference_size: it is a compiler constant (64 here),
//   not a reading of this machine.
constexpr std::size_t kCacheLineBytes = 128;

// 64 B: the unit one core's L1 pulls in at a time.
// - A slot keeps its stamp + 7 payload words inside one 64 B unit, so a reader moves one unit
//   per message.
// - measured: a payload spread over two units costs about 40% more per hop (DESIGN.md,
//   Results: "Payload in one 64 B line vs two", the Copy15 variant).
constexpr std::size_t kL1LineBytes = 64;

constexpr std::size_t kWordBytes = sizeof(std::uint64_t);  // every shared field is one 8 B word

// 16384 slots x 128 B = a 2 MB ring. At 1 M msg/s a reader can fall ~16 ms behind before the
// writer laps it. Must be a power of two: the slot index is seq & (slot count - 1).
constexpr std::size_t kDefaultSlotCount = 16384;

// 7 words x 8 B = 56 B of payload + the 8 B stamp = exactly one 64 B unit (see kL1LineBytes).
constexpr std::size_t kDefaultPayloadWords = 7;

// 6 is what fits, not a tuned number: 24 B snapshot header + 6 levels x 8 B x 2 sides = 120 B,
// plus the record's 8 B version = one 128 B snapshot record.
constexpr std::size_t kTopLevelsPerSide = 6;

// Bump by hand when a shared field moves or changes type without changing any struct size:
// the layout hash only sees sizes, so it cannot notice that on its own.
constexpr std::uint32_t kLayoutVersion = 5;

// Defaults and timings (not in the layout hash).

constexpr std::size_t kPageSize = 16384;                 // Apple Silicon pages are 16 KB, not 4 KB
constexpr std::uint32_t kDefaultInstrumentCount = 1024;  // snapshot table: 1024 x 128 B = 128 KB
// Bus instrument ids are uint16_t, so 65536 is every id. The book's own limit is 65534
// (book/order_event.hpp): a different layer, a different limit.
constexpr std::uint32_t kMaxInstrumentCount = 65536;

// Writer cadence.
// - The head hint (the next seq to publish, kept in the control block) is stored once per 64
//   publishes: one extra store per 64, and a new or lapped reader starts at most 63 messages
//   behind the true head.
// - It is a cap: a tiny test ring uses half its slot count instead
//   (RingWriter::kPublishesPerHeadHint).
constexpr std::uint64_t kMaxPublishesPerHeadHint = 64;
// The clock is read once per 1024 publishes to see whether a heartbeat is due, which keeps a
// clock read off almost every publish.
constexpr std::uint64_t kPublishesPerHeartbeatCheck = 1024;

constexpr std::size_t kMaxShmNameLength = 31;  // macOS limit (PSHMNAMLEN), counting the '/'

// Liveness timing, all in steady_clock_ns() nanoseconds.
constexpr std::uint64_t kHeartbeatPeriodNs = 1'000'000;     // writer refreshes at least every 1 ms
constexpr std::uint64_t kHeartbeatTimeoutNs = 100'000'000;  // 100 ms quiet: a reader starts probing
// A new writer keeps retrying the lock for 10 ms: a reader's probe holds a shared lock for a few
// microseconds, so one failed attempt does not prove another writer exists.
constexpr std::uint64_t kWriterLockTimeoutNs = 10'000'000;
// A quiet reader probes the writer (state, heartbeat, then flock) and tries a re-attach at most
// once per millisecond.
constexpr std::uint64_t kWriterProbeIntervalNs = 1'000'000;
// - Caveat: if steady_clock counts through system sleep, a reader can briefly see a sleeping
//   writer as stalled (the flock is still held), never as down.

// A snapshot read tries once, then retries up to 256 times (one spin_pause before each), so it
// gives up after 1 + 256 attempts, a few microseconds in all.
// - A live writer finishes an update far sooner, so hitting the cap means the writer stopped
//   mid-update; the cap exists so that cannot hang a reader. Not tuned.
constexpr std::uint32_t kSnapshotReadMaxRetries = 256;

}  // namespace mdbus
