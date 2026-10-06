#pragma once

// Compile-time constants of the bus, in two groups:
// - Shared-memory shape: the writer stores the layout version and the ring's geometry in the
//   segment header, so a reader built with different values refuses to attach
//   (Status::LayoutMismatch, segment_format.hpp).
// - Defaults and timings: not checked. A writer and a reader built with different values still
//   attach; keep them equal by building both from one tree.

#include <cstddef>
#include <cstdint>

// The tick clock (cntvct_el0), the isb pauses and the 128 B line size are all M1 facts.
#if !defined(__aarch64__) || !defined(__APPLE__)
#error "mdbus targets Apple Silicon (arm64 macOS) only"
#endif

namespace mdbus {

// Shared-memory shape (checked at attach).

// 128 B: the cache-line size macOS reports on M1 (sysctl hw.cachelinesize).
// - Every shared struct that a different core writes gets its own 128 B line, so two of them
//   never share a line (false sharing: two cores writing different data in one line).
// - Why not std::hardware_destructive_interference_size: it is a compiler constant (64 here),
//   not a reading of this machine.
constexpr std::size_t kCacheLineBytes = 128;

// 64 B: the unit one core's L1 pulls in at a time.
// - A slot keeps its stamp + 7 payload words inside one 64 B unit, so a reader moves one unit
//   per message.
// - Measured: a payload spread over two units costs about 48% more per hop (DESIGN.md §7,
//   "One 64 B unit per message", the Copy15 variant: 86.9 -> 128.9 ns).
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

// Bump by hand on any change to what lives in shared memory: a field added, moved or retyped, a
// message added or changed, flag or enum values, the stamp or seqlock encoding. The header also
// carries the ring's geometry, so a different slot count or slot size is caught without a bump.
constexpr std::uint32_t kLayoutVersion = 6;

// Defaults and timings (not checked).

constexpr std::size_t kPageSize = 16384;                 // Apple Silicon pages are 16 KB, not 4 KB
constexpr std::uint32_t kDefaultInstrumentCount = 1024;  // snapshot table: 1024 x 128 B = 128 KB
// Bus instrument ids are uint16_t, so 65536 is every id. The book's own limit is 65534
// (book/order_event.hpp): a different layer, a different limit.
constexpr std::uint32_t kMaxInstrumentCount = 65536;

// Writer cadence.
// - The head hint (ring.hpp) is stored once per 64 publishes: one extra store per 64, and a new
//   or lapped reader starts at most 63 messages behind the true head.
// - It is a cap: a tiny test ring uses half its slot count instead
//   (RingWriter::kPublishesPerHeadHint).
constexpr std::uint64_t kMaxPublishesPerHeadHint = 64;
// The clock is read once per 1024 publishes to see whether a heartbeat is due, which keeps a
// clock read off almost every publish.
constexpr std::uint64_t kPublishesPerHeartbeatCheck = 1024;

constexpr std::size_t kMaxShmNameLength = 31;  // macOS limit (PSHMNAMLEN), counting the '/'

// Liveness timing, all in steady_clock_ns() nanoseconds.
constexpr std::uint64_t kHeartbeatPeriodNs = 1'000'000;     // at most one heartbeat store per 1 ms
// 100 ms with no message and no fresh heartbeat: the reader reports the writer Down.
constexpr std::uint64_t kHeartbeatTimeoutNs = 100'000'000;

// A snapshot read tries once, then retries up to 256 times (one spin_pause before each): a time
// budget, about 2.6 us for the 257 attempts (about 10 ns each, most of it the isb). Not tuned.
// - A writer updating normally finishes far sooner, but one that is descheduled mid-update for
//   longer makes readers give up while it is alive (SnapshotReadStatus::GaveUp).
constexpr std::uint32_t kSnapshotReadMaxRetries = 256;

// Hashing.

// 2^64 / golden ratio (1.618...), a common choice for multiplicative hashing.
// - Multiplying mixes each input bit into the bits above it, so the top bits of a product are
//   the well-mixed ones.
// - Odd, so the multiply never loses information.
constexpr std::uint64_t kGoldenRatioMultiplier = 0x9E3779B97F4A7C15;

}  // namespace mdbus
