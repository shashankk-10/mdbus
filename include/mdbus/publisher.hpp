#pragma once

// Publisher: encode a message into slot words, publish it, keep each instrument's snapshot
// current.
// - The feed handler's write path: FeedBook -> publish_book_output() -> Publisher ->
//   RingWriter::publish(). BusWriter owns the Publisher of a named bus (publisher()).
// - The only code that writes the ring and the snapshot table; readers see its work through
//   Consumer.
// - latency_start_ticks: the 24 MHz tick a latency measurement starts from (the intended send
//   tick in benches, the packet receive tick in the feed handler). 0 means "not measured".
// - heartbeat: a steady-clock time stored in the control block so readers can tell a quiet
//   writer from a dead one (writer_liveness.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/clock.hpp"
#include "mdbus/ring.hpp"
#include "mdbus/segment_format.hpp"
#include "mdbus/snapshot_table.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// Stores the publish tick (word 1) into encoded words, after the checksum is computed.
// - read_ticks_after() takes the checksum's word as an input. A plain read_ticks() keeps its
//   place among memory accesses but not among register arithmetic, so the compiler once moved
//   it above the checksum and the measured hop grew by the checksum's cost (DESIGN.md Method,
//   "Three things the method caught in itself", item 3).
// - Shared with the mutex writer in baseline/bus_variants.hpp, so both take the tick the same way.
template <std::size_t PayloadWordCount>
inline void write_publish_ticks(std::array<std::uint64_t, PayloadWordCount>& words) {
  words[kPublishTicksWord] = read_ticks_after(words[kChecksumWord]);
}

// The writer side of one bus segment, used from one thread.
template <class Layout = BusLayout<>>
class Publisher {
 public:
  using PayloadWords = typename Layout::PayloadWords;

 private:
  SegmentPointers<Layout> segment_pointers{};  // control block and instrument count
  RingWriter<StdAtomics, Layout> ring_writer;
  SnapshotTable<StdAtomics> snapshot_table;

 public:
  Publisher() = default;
  explicit Publisher(const SegmentPointers<Layout>& pointers)
      : segment_pointers(pointers),
        ring_writer(pointers.slots, pointers.control),
        snapshot_table(pointers.snapshot_records) {}

  // Builds the payload words of message `seq`: header, body, checksum. publish_ticks stays 0.
  // - The payload is zeroed first, so padding and the unused body tail are always 0 and the
  //   checksum of a message is always the same.
  // - The checksum covers every word except publish_ticks and the checksum itself.
  // Example: encode_payload(42, 7, Trade{price 10025, qty 300, kBuyerAggressor}, 1000):
  //   word 0     = 1000                   latency_start_ticks
  //   word 1     = 0                      publish_ticks (write_publish_ticks fills it in)
  //   word 2     = checksum << 32 | 0x0002'0007
  //                                       instrument_id 7, type_id 2 (Trade), padding 0
  //   word 3     = 0x0000'012c'0000'2729  price 10025 (low half), qty 300 (high half)
  //   word 4     = 0                      aggressor 0 (kBuyerAggressor) + padding
  //   words 5, 6 = 0                      unused body tail
  template <class Message>
  static PayloadWords encode_payload(std::uint64_t seq, std::uint16_t instrument_id,
                                     const Message& message, std::uint64_t latency_start_ticks) {
    static_assert(Layout::Schema::template kContains<Message>,
                  "message type is not in this schema");
    Payload<Layout::kPayloadWords> payload{};
    payload.header.latency_start_ticks = latency_start_ticks;
    payload.header.instrument_id = instrument_id;
    payload.header.type_id = Message::kTypeId;
    std::memcpy(payload.body, &message, sizeof(Message));
    payload.header.checksum = payload_checksum(payload, seq);

    PayloadWords words;
    std::memcpy(words.data(), &payload, sizeof words);
    return words;
  }

  // Stores WriterState::Running and a first heartbeat; BusWriter::open() calls it last.
  void mark_running() {
    store_writer_state(*segment_pointers.control, WriterState::Running);
    ring_writer.write_heartbeat(steady_clock_ns());
  }

  // Clean shutdown: a last heartbeat, then Exited, so readers report Down at once instead of
  // waiting out the heartbeat timeout.
  void mark_exited() {
    ring_writer.write_heartbeat(steady_clock_ns());
    store_writer_state(*segment_pointers.control, WriterState::Exited);
  }

  // Publishes `message` as the next seq and returns that seq, with no latency measurement
  // (latency_start_ticks and publish_ticks stay 0).
  // - Once inlined, the payload words are built in registers and stored straight into the
  //   slot; nothing goes through a stack copy.
  // Example (a fresh bus): publish(7, Trade{10025, 300, kBuyerAggressor}) returns 0; slot 0's
  // stamp goes 0 -> 1 -> 2; next_seq() is now 1.
  template <class Message>
  std::uint64_t publish(std::uint16_t instrument_id, const Message& message) {
    const std::uint64_t seq = ring_writer.next_seq();
    ring_writer.publish(encode_payload(seq, instrument_id, message, 0));
    return seq;
  }

  // publish() plus the two latency ticks: latency_start_ticks from the caller, publish_ticks
  // read here.
  // - publish_ticks is read after the checksum and right before the odd stamp store, so the
  //   time from publish_ticks to the even store is the slot protocol and nothing else.
  // - Why a separate entry point and not a flag on publish(): the benchmark writer passes a
  //   runtime value, and a flag would put a branch on every publish.
  // Example (seq 1 on a bus, latency_start_ticks taken with read_ticks() just before):
  //   publish_with_latency_start(7, trade, 22862364843346) returns 1; slot 1's stamp is 4,
  //   word 0 = 22862364843346, word 1 = 22862364843347 (the counter, one tick later).
  template <class Message>
  std::uint64_t publish_with_latency_start(std::uint16_t instrument_id, const Message& message,
                                           std::uint64_t latency_start_ticks) {
    const std::uint64_t seq = ring_writer.next_seq();
    PayloadWords words = encode_payload(seq, instrument_id, message, latency_start_ticks);
    write_publish_ticks(words);
    ring_writer.publish(words);
    return seq;
  }

  // Publishes `message`, then writes `snapshot` as instrument_id's snapshot, stamped with the
  // seq just published (last_included_seq) and the id.
  // - Slot first, then snapshot: lazy recovery (consumer.hpp, step 2) relies on this order.
  //   The next slot's even store is a release after the snapshot's, so a reader that acquires
  //   any later slot also sees this snapshot.
  // - latency_start_ticks != 0 measures latency as publish_with_latency_start() does.
  // - An id at or past the instrument count aborts before anything is published: the write
  //   would land past the end of the snapshot table, in memory every reader maps.
  // Example (seq 2 on a bus where instrument 7's snapshot was never written):
  //   publish_and_update_snapshot(7, delta, snapshot) returns 2; slot 2's stamp is 6;
  //   record 7's version goes 0 -> 1 -> 2 and holds last_included_seq 2, instrument_id 7.
  //   The same call again returns 3 and leaves version 4.
  template <class Message>
  std::uint64_t publish_and_update_snapshot(std::uint16_t instrument_id, const Message& message,
                                            InstrumentSnapshot snapshot,
                                            std::uint64_t latency_start_ticks = 0) {
    check_or_abort(instrument_id < segment_pointers.instrument_count,
                   "publish_and_update_snapshot: instrument id past the table");
    std::uint64_t seq;
    if (latency_start_ticks != 0)
      seq = publish_with_latency_start(instrument_id, message, latency_start_ticks);
    else
      seq = publish(instrument_id, message);
    snapshot.last_included_seq = seq;
    snapshot.instrument_id = instrument_id;
    snapshot_table.write(instrument_id, snapshot);
    return seq;
  }

  // Bench-only: publishes words encoded beforehand with encode_payload(), so a benchmark
  // measures the ring and not the encoding.
  void publish_encoded_words(const PayloadWords& words) {
    ring_writer.publish(words);
  }

  // For the writer's idle loop, so a quiet writer still looks alive.
  void heartbeat_if_due() {
    ring_writer.heartbeat_if_due();
  }

  std::uint64_t next_seq() const {
    return ring_writer.next_seq();
  }
};

}  // namespace mdbus
