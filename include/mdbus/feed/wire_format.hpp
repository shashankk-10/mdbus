#pragma once

// The exchange feed's bytes: how the simulator packs order events into UDP packets and how the
// feed handler unpacks them.
// - Used by sim/exchange_sim.cpp (PacketBuilder) and src/feed_handler.cpp (decode_packet,
//   SequenceGapTracker). Data flow: exchange -> [these bytes] -> feed handler -> book -> bus.
// - Shaped like NASDAQ ITCH 5.0 (the exchange's order-by-order message set, one type letter per
//   message) inside MoldUDP64 (NASDAQ's UDP framing: a sequence-numbered header, then each
//   message behind a 2 B length).
// - Kept: the type letters, packed big-endian fields, the length before each message. Dropped:
//   MoldUDP64's 10 B session name (a 16 B header, not 20 B), retransmission, and the execute's
//   8 B match number. ITCH's 2 B stock locate + 2 B tracking number + 6 B time become a 2 B
//   instrument id + 8 B time, so A, X and U keep ITCH's sizes (36, 23, 35 B).
// - The book never sees these bytes: decode_packet turns each message into a book::OrderEvent,
//   whose fields sit at their natural alignment.
//
// One packet on the wire (big-endian throughout):
//   0..7    first_seq      seq of the first message, counting from 1
//   8..11   message_count  0 to kMaxMessagesPerPacket
//   12..15  end_of_stream  1 on the last packet of a run, else 0
//   16..    message_count times: a 2 B length, then that many bytes of message

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/book/order_event.hpp"

namespace mdbus::wire {

// Our choice, not an ITCH rule; the biggest packet is then 396 B, far below a 1500 B Ethernet
// frame.
constexpr std::uint32_t kMaxMessagesPerPacket = 10;

// Byte order.
// - The M1 is little-endian, the wire big-endian. The compiler turns each swap into one
//   instruction (rev).
// - to_big_endian on the way out, from_big_endian on the way in: the same swap, named by its
//   direction at the call site.

inline std::uint16_t to_big_endian(std::uint16_t value) {
  return __builtin_bswap16(value);
}

inline std::uint32_t to_big_endian(std::uint32_t value) {
  return __builtin_bswap32(value);
}

inline std::uint64_t to_big_endian(std::uint64_t value) {
  return __builtin_bswap64(value);
}

inline std::int32_t to_big_endian(std::int32_t value) {
  return static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(value)));
}

template <class T>
T from_big_endian(T value) {
  return to_big_endian(value);
}

// Message layouts.
// - packed: the compiler adds no padding, so every field sits at a fixed offset and sizeof is
//   the size on the wire.
// - A packed field may sit at an unaligned address, so fields are only read and written by
//   value, never through a pointer or reference, and whole structs move in and out of the
//   buffer with memcpy.

// The 16 B header at the start of every packet.
// - The end-of-stream packet has message_count 0 and the seq that would come next.
struct __attribute__((packed)) PacketHeader {
  std::uint64_t first_seq;
  std::uint32_t message_count;
  std::uint32_t end_of_stream;
};
static_assert(sizeof(PacketHeader) == 8 + 4 + 4);

// ITCH 5.0 letters: A Add Order, X Order Cancel, E Order Executed, U Order Replace.
constexpr std::uint8_t kAddMessageType = 'A';
constexpr std::uint8_t kCancelMessageType = 'X';
constexpr std::uint8_t kExecuteMessageType = 'E';
constexpr std::uint8_t kReplaceMessageType = 'U';

struct __attribute__((packed)) AddMessage {
  std::uint8_t type;  // 'A'
  std::uint16_t instrument_id;
  std::uint64_t exchange_time_ns;
  std::uint64_t order_id;
  std::uint8_t side;  // a book::Side value: 0 bid, 1 ask (ITCH sends 'B' / 'S')
  std::int32_t price;
  std::uint32_t qty;
  char symbol[8];
};
static_assert(sizeof(AddMessage) == 1 + 2 + 8 + 8 + 1 + 4 + 4 + 8);  // 36

// Cancel ('X') and execute ('E') share a layout. qty is the size removed or filled; a cancel's
// 0 means all of it.
struct __attribute__((packed)) CancelOrExecuteMessage {
  std::uint8_t type;
  std::uint16_t instrument_id;
  std::uint64_t exchange_time_ns;
  std::uint64_t order_id;
  std::uint32_t qty;
};
static_assert(sizeof(CancelOrExecuteMessage) == 1 + 2 + 8 + 8 + 4);  // 23

// Ends order_id and adds new_order_id on the same side with a new price and size.
struct __attribute__((packed)) ReplaceMessage {
  std::uint8_t type;  // 'U'
  std::uint16_t instrument_id;
  std::uint64_t exchange_time_ns;
  std::uint64_t order_id;  // the order being replaced
  std::uint64_t new_order_id;
  std::int32_t price;
  std::uint32_t qty;
};
static_assert(sizeof(ReplaceMessage) == 1 + 2 + 8 + 8 + 8 + 4 + 4);  // 35

// Big-endian length before each message, so a receiver can skip a type it does not know.
constexpr std::size_t kMessageLengthFieldBytes = 2;

static_assert(sizeof(AddMessage) >= sizeof(ReplaceMessage), "an add is the largest message");
// 16 + 10 x (2 + 36) = 396
constexpr std::size_t kMaxPacketBytes =
    sizeof(PacketHeader) + kMaxMessagesPerPacket * (kMessageLengthFieldBytes + sizeof(AddMessage));

// Encoding.

// Builds one packet in place: start(), then up to kMaxMessagesPerPacket add()s; or
// start_end_of_stream() alone.
// - The header is rewritten on every add, so data() / size() are a valid packet after any call
//   and there is no finish() to forget. It costs a 16 B copy per message, on the sending side.
// - The caller stops at kMaxMessagesPerPacket messages; add() does not check the count.
class PacketBuilder {
 private:
  std::uint8_t bytes[kMaxPacketBytes]{};
  std::size_t bytes_used = sizeof(PacketHeader);
  std::uint64_t first_seq = 0;
  std::uint32_t messages_added = 0;
  std::uint32_t end_of_stream_flag = 0;  // the header's end_of_stream: 0 or 1

 public:
  // Empties the builder for a packet whose first message will be packet_first_seq.
  void start(std::uint64_t packet_first_seq) {
    begin(packet_first_seq, 0);
  }

  // The end-of-stream packet: no messages, and the seq that would come next.
  // Example: start_end_of_stream(51) -> size() 16, bytes
  //   00 00 00 00 00 00 00 33 | 00 00 00 00 | 00 00 00 01
  void start_end_of_stream(std::uint64_t packet_first_seq) {
    begin(packet_first_seq, 1);
  }

  // Appends event in its type's layout, behind its 2 B length. An event of no known type adds
  // nothing.
  // Example (after start(1); a cancel of order 42 on instrument 3, qty 0, time
  // 34'200'000'000'250 ns):
  //   size() 16 -> 41 (2 B length + 23 B message), message_count() 0 -> 1
  //   bytes 8..11 (message_count): 00 00 00 01
  //   bytes 16..40: 00 17 | 58 | 00 03 | 00 00 1f 1a ce d9 f0 fa | 00 00 00 00 00 00 00 2a |
  //                 00 00 00 00      (length 23, 'X', instrument, time, order id, qty)
  //   then an add -> size() 79 (+ 2 + 36), message_count() 2
  void add(const book::OrderEvent& event) {
    switch (event.type) {
      case book::EventType::Add: {
        AddMessage add_message{};
        add_message.type = kAddMessageType;
        add_message.instrument_id = to_big_endian(event.instrument_id);
        add_message.exchange_time_ns = to_big_endian(event.exchange_time_ns);
        add_message.order_id = to_big_endian(event.order_id);
        add_message.side = static_cast<std::uint8_t>(event.side);
        add_message.price = to_big_endian(event.price);
        add_message.qty = to_big_endian(event.qty);
        std::memcpy(add_message.symbol, event.symbol.data(), sizeof add_message.symbol);
        append(&add_message, sizeof add_message);
        break;
      }
      case book::EventType::Cancel:
      case book::EventType::Execute: {
        CancelOrExecuteMessage cancel_or_execute_message{};
        cancel_or_execute_message.type =
            event.type == book::EventType::Cancel ? kCancelMessageType : kExecuteMessageType;
        cancel_or_execute_message.instrument_id = to_big_endian(event.instrument_id);
        cancel_or_execute_message.exchange_time_ns = to_big_endian(event.exchange_time_ns);
        cancel_or_execute_message.order_id = to_big_endian(event.order_id);
        cancel_or_execute_message.qty = to_big_endian(event.qty);
        append(&cancel_or_execute_message, sizeof cancel_or_execute_message);
        break;
      }
      case book::EventType::Replace: {
        ReplaceMessage replace_message{};
        replace_message.type = kReplaceMessageType;
        replace_message.instrument_id = to_big_endian(event.instrument_id);
        replace_message.exchange_time_ns = to_big_endian(event.exchange_time_ns);
        replace_message.order_id = to_big_endian(event.order_id);
        replace_message.new_order_id = to_big_endian(event.new_order_id);
        replace_message.price = to_big_endian(event.price);
        replace_message.qty = to_big_endian(event.qty);
        append(&replace_message, sizeof replace_message);
        break;
      }
      default:
        return;
    }
    ++messages_added;
    write_header();
  }

  const std::uint8_t* data() const {
    return bytes;
  }

  std::size_t size() const {
    return bytes_used;
  }

  std::uint32_t message_count() const {
    return messages_added;
  }

 private:
  void begin(std::uint64_t packet_first_seq, std::uint32_t end_of_stream) {
    first_seq = packet_first_seq;
    messages_added = 0;
    end_of_stream_flag = end_of_stream;
    bytes_used = sizeof(PacketHeader);
    write_header();
  }

  // Writes the big-endian length, then the message, at the end of the packet.
  void append(const void* message, std::size_t length) {
    const std::uint16_t wire_length = to_big_endian(static_cast<std::uint16_t>(length));
    std::memcpy(bytes + bytes_used, &wire_length, kMessageLengthFieldBytes);
    std::memcpy(bytes + bytes_used + kMessageLengthFieldBytes, message, length);
    bytes_used += kMessageLengthFieldBytes + length;
  }

  void write_header() {
    PacketHeader header{};
    header.first_seq = to_big_endian(first_seq);
    header.message_count = to_big_endian(messages_added);
    header.end_of_stream = to_big_endian(end_of_stream_flag);
    std::memcpy(bytes, &header, sizeof header);
  }
};

// Decoding.

// A received packet, decoded.
// - first_seq and message_count are the header's.
// - events[0..event_count) are the decoded messages, in packet order.
// - skipped_unknown_messages counts messages of a type we do not know. They still use up their
//   seqs: after a successful decode, event_count + skipped_unknown_messages == message_count.
struct DecodedPacket {
  std::uint64_t first_seq = 0;
  std::uint32_t message_count = 0;
  bool end_of_stream = false;
  std::uint32_t event_count = 0;
  std::uint32_t skipped_unknown_messages = 0;
  book::OrderEvent events[kMaxMessagesPerPacket];
};

// Decodes one message into the next free entry of out.events, or counts it as skipped if its
// type is unknown. False when a known type has the wrong length.
// - The caller guarantees length >= 1, so message_bytes[0] (the type letter) is inside it.
inline bool decode_one_message(const std::uint8_t* message_bytes, std::size_t length,
                               DecodedPacket& out) {
  book::OrderEvent& event = out.events[out.event_count];
  event = book::OrderEvent{};

  switch (message_bytes[0]) {
    case kAddMessageType: {
      if (length != sizeof(AddMessage)) return false;
      AddMessage add_message;
      std::memcpy(&add_message, message_bytes, sizeof add_message);
      event.type = book::EventType::Add;
      event.instrument_id = from_big_endian(add_message.instrument_id);
      event.exchange_time_ns = from_big_endian(add_message.exchange_time_ns);
      event.order_id = from_big_endian(add_message.order_id);
      // Copied as sent: the book refuses a side other than 0 or 1.
      event.side = static_cast<book::Side>(add_message.side);
      event.price = from_big_endian(add_message.price);
      event.qty = from_big_endian(add_message.qty);
      std::memcpy(event.symbol.data(), add_message.symbol, sizeof add_message.symbol);
      break;
    }
    case kCancelMessageType:
    case kExecuteMessageType: {
      if (length != sizeof(CancelOrExecuteMessage)) return false;
      CancelOrExecuteMessage cancel_or_execute_message;
      std::memcpy(&cancel_or_execute_message, message_bytes, sizeof cancel_or_execute_message);
      event.type = cancel_or_execute_message.type == kCancelMessageType
                       ? book::EventType::Cancel
                       : book::EventType::Execute;
      event.instrument_id = from_big_endian(cancel_or_execute_message.instrument_id);
      event.exchange_time_ns = from_big_endian(cancel_or_execute_message.exchange_time_ns);
      event.order_id = from_big_endian(cancel_or_execute_message.order_id);
      event.qty = from_big_endian(cancel_or_execute_message.qty);
      break;
    }
    case kReplaceMessageType: {
      if (length != sizeof(ReplaceMessage)) return false;
      ReplaceMessage replace_message;
      std::memcpy(&replace_message, message_bytes, sizeof replace_message);
      event.type = book::EventType::Replace;
      event.instrument_id = from_big_endian(replace_message.instrument_id);
      event.exchange_time_ns = from_big_endian(replace_message.exchange_time_ns);
      event.order_id = from_big_endian(replace_message.order_id);
      event.new_order_id = from_big_endian(replace_message.new_order_id);
      event.price = from_big_endian(replace_message.price);
      event.qty = from_big_endian(replace_message.qty);
      break;
    }
    default:
      ++out.skipped_unknown_messages;
      return true;
  }
  ++out.event_count;
  return true;
}

// Decodes a whole datagram into out. False for a malformed packet, which the caller drops whole.
// - Malformed: shorter than its header, more messages than a packet holds, a zero-length
//   message, a message running past the end, a known type with the wrong length, or bytes left
//   over after the last message.
// - On false, out is partly filled and means nothing.
// Example (the 79 B packet from PacketBuilder::add: start(1), a cancel, then an add):
//   size 79                         -> true; first_seq 1, message_count 2, event_count 2
//   size 78                         -> false (the add runs past the end)
//   the cancel's type byte set to Z -> true; event_count 1 (the add), skipped_unknown_messages 1
//   start_end_of_stream(51), size 16 -> true; first_seq 51, message_count 0, end_of_stream true
inline bool decode_packet(const std::uint8_t* data, std::size_t size, DecodedPacket& out) {
  // 1. The header.
  if (size < sizeof(PacketHeader)) return false;
  PacketHeader header;
  std::memcpy(&header, data, sizeof header);
  out.first_seq = from_big_endian(header.first_seq);
  out.message_count = from_big_endian(header.message_count);
  // Read without a swap: a zero test does not depend on byte order.
  out.end_of_stream = header.end_of_stream != 0;
  out.event_count = 0;
  out.skipped_unknown_messages = 0;
  if (out.message_count > kMaxMessagesPerPacket) return false;

  // 2. Each message: its length, then its bytes. A zero length is refused because the message
  //    would have no type letter to read.
  std::size_t read_offset = sizeof(PacketHeader);
  for (std::uint32_t i = 0; i < out.message_count; ++i) {
    if (size - read_offset < kMessageLengthFieldBytes) return false;
    std::uint16_t wire_length = 0;
    std::memcpy(&wire_length, data + read_offset, kMessageLengthFieldBytes);
    const std::size_t length = from_big_endian(wire_length);
    read_offset += kMessageLengthFieldBytes;
    if (length == 0 || size - read_offset < length) return false;
    if (!decode_one_message(data + read_offset, length, out)) return false;
    read_offset += length;
  }

  // 3. Nothing may follow the last message.
  return read_offset == size;
}

// Checking packet order.

// Classifies each packet by its first_seq against the next seq expected.
// - Seqs count messages, so one comparison is enough.
// - Nothing is retransmitted, so after a gap the book stays suspect for the rest of the run.
// - A receiver that joins late sees its first packet as a gap.
class SequenceGapTracker {
 private:
  std::uint64_t next_expected_seq = 1;
  std::uint64_t gaps_seen = 0;
  std::uint64_t messages_missing = 0;

 public:
  enum PacketOrder { kInOrder, kGap, kDuplicate };

  // Classifies one packet and moves the expected seq past it (kDuplicate leaves it).
  // - kDuplicate: first_seq is behind; the packet is a repeat or arrived late. Either way its
  //   messages are already counted as applied or missing, so the caller ignores it.
  // Example (a new tracker, 1 message per packet):
  //   check_packet(1, 1) -> kInOrder    next expected 2
  //   check_packet(2, 1) -> kInOrder    next expected 3
  //   check_packet(5, 1) -> kGap        seqs 3 and 4 lost: gap_count() 1, missing 2; next 6
  //   check_packet(3, 1) -> kDuplicate  3 < 6, nothing changes
  PacketOrder check_packet(std::uint64_t first_seq, std::uint32_t message_count) {
    if (first_seq < next_expected_seq) return kDuplicate;
    PacketOrder order = kInOrder;
    if (first_seq > next_expected_seq) {
      ++gaps_seen;
      messages_missing += first_seq - next_expected_seq;
      order = kGap;
    }
    next_expected_seq = first_seq + message_count;
    return order;
  }

  std::uint64_t gap_count() const {
    return gaps_seen;
  }

  std::uint64_t missing_message_count() const {
    return messages_missing;
  }

  bool has_seen_gap() const {
    return gaps_seen != 0;
  }
};

}  // namespace mdbus::wire
