#pragma once

// Publisher: encode a message into slot words, publish it, keep each instrument's snapshot
// current. The feed handler writes the ring and the snapshot table only through it; bench and
// test writers also store into slots or records directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/clock.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/ring.hpp"
#include "mdbus/segment_format.hpp"
#include "mdbus/snapshot_table.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// Stores the publish tick (word 1) into encoded words, after the checksum is computed.
// - read_ticks_after() takes the checksum's word as an input, which keeps the checksum out of
//   the timed window (clock.hpp says why a plain read_ticks() does not).
// - The bench writers call it too, so every writer takes the tick the same way.
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

  // A first heartbeat. The state word already reads Running: a fresh segment is all zeros.
  void mark_running() {
    ring_writer.write_heartbeat(steady_clock_ns());
  }

  // Clean shutdown: a last heartbeat, then Exited. Readers read the state word at every health
  // check, so they report Down at their next one, not after the 100 ms heartbeat timeout.
  void mark_exited() {
    ring_writer.write_heartbeat(steady_clock_ns());
    store_writer_state(*segment_pointers.control, WriterState::Exited);
  }

  // Publishes `message` as the next seq and returns that seq, with no latency measurement
  // (latency_start_ticks and publish_ticks stay 0).
  // - Once inlined, the payload words are built in registers and stored straight into the
  //   slot; nothing goes through a stack copy.
  // - A BookDelta does not compile here: a consumer that recovers later would trust a snapshot
  //   that lacks it. Deltas go through publish_and_update_snapshot.
  template <class Message>
  std::uint64_t publish(std::uint16_t instrument_id, const Message& message) {
    static_assert(!kIncludedInSnapshot<Message>,
                  "a BookDelta must go through publish_and_update_snapshot");
    return publish_unchecked(instrument_id, message);
  }

  // publish() plus the two latency ticks: latency_start_ticks from the caller, publish_ticks
  // read here.
  // - publish_ticks is read after the checksum and right before the odd stamp store, so the
  //   time from publish_ticks to the even store is the slot protocol and nothing else.
  // - Why a separate entry point and not a flag on publish(): a caller that never measures pays
  //   no branch. Where the choice is made per message (the feed handler stamps only a packet's
  //   first slot), it costs one branch per message: in publish_book_output for a trade, inside
  //   publish_and_update_snapshot for a status or a delta.
  template <class Message>
  std::uint64_t publish_with_latency_start(std::uint16_t instrument_id, const Message& message,
                                           std::uint64_t latency_start_ticks) {
    static_assert(!kIncludedInSnapshot<Message>,
                  "a BookDelta must go through publish_and_update_snapshot");
    return publish_with_latency_start_unchecked(instrument_id, message, latency_start_ticks);
  }

  // Publishes `message`, then writes `snapshot` as instrument_id's snapshot, stamped with the
  // seq just published (last_included_seq) and the id.
  // - Slot first, then snapshot, for latency: the delta is visible one snapshot write sooner.
  //   Recovery (consumer.hpp) needs only this snapshot complete before the next slot's release
  //   store, which one writer gives in either order: a reader that acquires any later slot also
  //   sees this snapshot.
  // - latency_start_ticks != 0 measures latency as publish_with_latency_start() does.
  // - An id at or past the instrument count aborts before anything is published: the write
  //   would land past the end of the snapshot table, in memory every reader maps.
  template <class Message>
  std::uint64_t publish_and_update_snapshot(std::uint16_t instrument_id, const Message& message,
                                            InstrumentSnapshot snapshot,
                                            std::uint64_t latency_start_ticks = 0) {
    check_or_abort(instrument_id < segment_pointers.instrument_count,
                   "publish_and_update_snapshot: instrument id past the table");
    std::uint64_t seq;
    if (latency_start_ticks != 0)
      seq = publish_with_latency_start_unchecked(instrument_id, message, latency_start_ticks);
    else
      seq = publish_unchecked(instrument_id, message);
    snapshot.last_included_seq = seq;
    snapshot.instrument_id = instrument_id;
    snapshot_table.write(instrument_id, snapshot);
    return seq;
  }

  // Benches and tests only: publishes words encoded beforehand with encode_payload(). A bench
  // then times the ring and not the encoding; a test can publish a delta's slot alone.
  void publish_encoded_words(const PayloadWords& words) {
    ring_writer.publish(words);
  }

  // For the writer's own loop, idle or busy without publishing, so it still looks alive.
  void heartbeat_if_due() {
    ring_writer.heartbeat_if_due();
  }

  std::uint64_t next_seq() const {
    return ring_writer.next_seq();
  }

 private:
  // publish() and publish_with_latency_start() without the BookDelta check, for
  // publish_and_update_snapshot, which writes the snapshot right after.
  template <class Message>
  std::uint64_t publish_unchecked(std::uint16_t instrument_id, const Message& message) {
    const std::uint64_t seq = ring_writer.next_seq();
    ring_writer.publish(encode_payload(seq, instrument_id, message, 0));
    return seq;
  }

  template <class Message>
  std::uint64_t publish_with_latency_start_unchecked(std::uint16_t instrument_id,
                                                     const Message& message,
                                                     std::uint64_t latency_start_ticks) {
    const std::uint64_t seq = ring_writer.next_seq();
    PayloadWords words = encode_payload(seq, instrument_id, message, latency_start_ticks);
    write_publish_ticks(words);
    ring_writer.publish(words);
    return seq;
  }
};

}  // namespace mdbus
