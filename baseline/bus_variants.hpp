#pragma once

// The bus variants that bench/bus_bench.cpp is built against, one binary per variant.
// - Base is the bus the system runs. Every other variant overrides exactly one of Base's
//   policies, so its comparison with Base measures that one decision. None of the others is
//   part of the system.
// - Built with -DMDBUS_VARIANT=<name>; the struct names are the CMake values and appear in the
//   results, so they stay as they are.
// - VariantBus<V>::Writer has open, next_seq, publish, publish_encoded_words, write_snapshot,
//   heartbeat_if_due.
// - VariantBus<V>::Reader has open, poll, resume_seq, idle, has_snapshots, read_snapshot.

#include <cstdint>
#include <string>

#include "baseline/mutex_ring.hpp"
#include "mdbus/bus_reader.hpp"
#include "mdbus/bus_writer.hpp"
#include "mdbus/ring.hpp"
#include "mdbus/snapshot_table.hpp"

namespace mdbus::baseline {

// Tags that pick the VariantBus specialisation.
struct StampProtocol {};  // the stamp ring
struct MutexProtocol {};  // one process-shared mutex over the same slots (mutex_ring.hpp)

// The variants.

namespace variants {
// The production policies.
struct Base {
  using Sync = StampProtocol;
  // 7 payload words: with the stamp, one 64 B half of the slot.
  static constexpr std::size_t kPayloadWords = kDefaultPayloadWords;
  static constexpr std::size_t kSlotBytes = kCacheLineBytes;  // 128 B, one M1 cache line
  // true: readers wait on a head stored after every publish (the HeadPoll variant).
  static constexpr bool kReadersPollHead = false;
};

struct Mutex : Base { using Sync = MutexProtocol; };                        // lock, not stamps
struct Pad64 : Base { static constexpr std::size_t kSlotBytes = 64; };      // half-line slots
struct Copy15 : Base { static constexpr std::size_t kPayloadWords = 15; };  // full-line payload
struct HeadPoll : Base { static constexpr bool kReadersPollHead = true; };  // poll a head
}  // namespace variants

// Copy15's stamp + 15 words still fill exactly one 128 B slot; Pad64's slot is 64 B, so two
// slots share a cache line.
static_assert(sizeof(Slot<StdAtomics, 15>) == kCacheLineBytes &&
              sizeof(Slot<StdAtomics, 7, 64>) == 64);

// Adapter for the stamp ring.

template <class Variant>
using VariantLayout =
    BusLayout<DefaultSchema, kDefaultSlotCount, Variant::kPayloadWords, Variant::kSlotBytes>;

// The steps of Publisher::publish_with_latency_start, which refuses a delta published without its
// snapshot (no bench reader rebuilds a book). It takes the Publisher, as that member did, so the
// paced writer's machine code is the same as when it called that member.
template <class Layout>
std::uint64_t publish_delta_with_latency_start(Publisher<Layout>& publisher,
                                               std::uint16_t instrument_id, const BookDelta& delta,
                                               std::uint64_t latency_start_ticks) {
  const std::uint64_t seq = publisher.next_seq();
  typename Layout::PayloadWords words =
      Publisher<Layout>::encode_payload(seq, instrument_id, delta, latency_start_ticks);
  write_publish_ticks(words);
  publisher.publish_encoded_words(words);
  return seq;
}

template <class Variant, class Sync = typename Variant::Sync>
struct VariantBus;

// The production BusWriter, RingReader and SnapshotTable behind the common interface.
template <class Variant>
struct VariantBus<Variant, StampProtocol> {
  using Layout = VariantLayout<Variant>;
  using PayloadWords = typename Layout::PayloadWords;
  static constexpr std::uint64_t kOpenTimeoutNs = 5'000'000'000;  // 5 s for the writer to appear

  struct Writer {
    BusWriter<Layout> bus_writer;
    SnapshotTable<StdAtomics> snapshot_table;

    bool open(const std::string& name, std::uint32_t instrument_count) {
      if (bus_writer.open(name, instrument_count) != Status::Ok) return false;
      snapshot_table = SnapshotTable<StdAtomics>(bus_writer.pointers().snapshot_records);
      return true;
    }

    std::uint64_t next_seq() {
      return bus_writer.publisher().next_seq();
    }

    // Publishes one delta. publish_ticks is read right before the odd stamp store, so the hop
    // starts inside the publish.
    void publish(std::uint16_t instrument_id, const BookDelta& delta,
                 std::uint64_t latency_start_ticks) {
      publish_delta_with_latency_start(bus_writer.publisher(), instrument_id, delta,
                                       latency_start_ticks);
      store_head();
    }

    void publish_encoded_words(const PayloadWords& words) {
      bus_writer.publisher().publish_encoded_words(words);
      store_head();
    }

    void write_snapshot(std::uint16_t instrument_id, const InstrumentSnapshot& snapshot) {
      snapshot_table.write(instrument_id, snapshot);
    }

    void heartbeat_if_due() {
      bus_writer.publisher().heartbeat_if_due();
    }

   private:
    // HeadPoll only: after the production publish, store the head for every message instead
    // of the head hint every 64.
    void store_head() {
      if constexpr (Variant::kReadersPollHead) {
        StdAtomics::store_release(bus_writer.pointers().control->head_hint.next_seq, next_seq());
      }
    }
  };

  struct Reader {
    BusReader<Layout> bus_reader;
    RingReader<StdAtomics, Layout> ring_reader;
    SnapshotTable<StdAtomics> snapshot_table;

    bool open(const std::string& name) {
      if (bus_reader.open(name, kOpenTimeoutNs) != Status::Ok) return false;
      ring_reader = RingReader<StdAtomics, Layout>(bus_reader.pointers().slots,
                                                   bus_reader.pointers().control);
      snapshot_table = SnapshotTable<StdAtomics>(bus_reader.pointers().snapshot_records);
      return true;
    }

    // RingReader::try_poll, plus the HeadPoll check in front of it.
    // - HeadPoll: the slot is read only once an acquire load of the head shows seq published.
    //   try_poll still checks the stamp, since the slot may have been lapped meanwhile.
    TryPollResult poll(std::uint64_t seq, PayloadWords& payload) {
      if constexpr (Variant::kReadersPollHead) {
        if (ring_reader.head_hint() <= seq) return TryPollResult::NotWrittenYet;
      }
      return ring_reader.try_poll(seq, payload);
    }

    // Where a reader restarts after a lap: the head hint.
    std::uint64_t resume_seq() {
      return ring_reader.head_hint();
    }

    void idle() {}  // spin: the next poll is the wait, for the E-core slow reader too

    bool has_snapshots() const {
      return true;
    }

    SnapshotReadResult read_snapshot(std::uint16_t instrument_id, InstrumentSnapshot& snapshot) {
      return snapshot_table.read(instrument_id, snapshot);
    }
  };
};

// Adapter for the mutex ring.

// Mutex: the same slots and payload words under one process-shared mutex; no snapshot table.
// - Variants without heartbeats or snapshots implement those methods as no-ops, so bus_bench
//   compiles against one interface.
template <class Variant>
struct VariantBus<Variant, MutexProtocol> {
  using Layout = VariantLayout<Variant>;
  using PayloadWords = typename Layout::PayloadWords;
  using Ring = MutexRing<Layout>;

  struct Writer {
    SharedMemoryMapping segment;
    Ring ring;
    std::uint64_t seq = 0;

    // Creates a fresh segment, removing any left by an earlier run.
    bool open(const std::string& name, std::uint32_t) {
      BusPaths paths;
      if (!make_bus_paths(name, paths)) return false;
      shm_unlink(paths.segment_name.c_str());
      const Status created = segment.create(paths.segment_name, Ring::kSize);
      if (created != Status::Ok) return false;
      return ring.create(segment.address());
    }

    std::uint64_t next_seq() {
      return seq;
    }

    // Encodes the same payload words the stamp ring would publish. publish_ticks is written
    // right before the lock, so the hop includes acquiring it.
    void publish(std::uint16_t instrument_id, const BookDelta& delta,
                 std::uint64_t latency_start_ticks) {
      PayloadWords words =
          Publisher<Layout>::encode_payload(seq, instrument_id, delta, latency_start_ticks);
      ++seq;
      write_publish_ticks(words);
      ring.publish(words);
    }

    void publish_encoded_words(const PayloadWords& words) {
      ++seq;
      ring.publish(words);
    }

    void write_snapshot(std::uint16_t, const InstrumentSnapshot&) {}
    void heartbeat_if_due() {}
  };

  struct Reader {
    SharedMemoryMapping segment;
    Ring ring;

    bool open(const std::string& name) {
      BusPaths paths;
      if (!make_bus_paths(name, paths)) return false;
      // Read-write: polling takes the lock, which writes it.
      const Status opened = segment.open(paths.segment_name, Access::ReadWrite, Ring::kSize);
      if (opened != Status::Ok) return false;
      ring.attach(segment.address());
      return true;
    }

    TryPollResult poll(std::uint64_t seq, PayloadWords& payload) {
      return ring.poll(seq, payload);
    }

    std::uint64_t resume_seq() {
      return ring.next_seq();
    }

    void idle() {}  // polling is the wait: every poll takes the lock

    bool has_snapshots() const {
      return false;
    }

    SnapshotReadResult read_snapshot(std::uint16_t, InstrumentSnapshot&) {
      return {SnapshotReadStatus::NeverWritten, 0};
    }
  };
};

}  // namespace mdbus::baseline
