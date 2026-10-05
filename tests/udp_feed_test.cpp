// The feed's wire format, sequence check and loopback multicast link.
// - Generated events survive encode and decode unchanged; the bytes match a copy written by
//   hand, big-endian and packed.
// - Malformed packets are refused whole, unknown message types skipped, and only multicast
//   addresses accepted.
// - SequenceGapTracker classifies in-order, duplicate, gapped and late-join packets.
// - Packets sent to a group arrive intact and in order at a receiver on this machine, and a
//   datagram larger than any packet arrives whole.

#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "mdbus/clock.hpp"
#include "mdbus/feed/multicast_socket.hpp"
#include "mdbus/feed/wire_format.hpp"
#include "sim/order_event_generator.hpp"
#include "test_harness.hpp"

using namespace mdbus;
using namespace mdbus::wire;

namespace {

constexpr std::uint64_t kReceiveTimeoutNs = 2'000'000'000;

std::vector<book::OrderEvent> generated(std::size_t n) {
  book::GeneratorConfig config;
  config.target_live_orders = 256;  // reach cancels, executions and replaces early
  book::OrderEventGenerator generator(config);
  std::vector<book::OrderEvent> events(n);
  generator.fill_events(events.data(), events.size());
  return events;
}

// A packet with one cancel of order 7 for 300, seq 1.
PacketBuilder one_cancel_packet() {
  book::OrderEvent e{};
  e.type = book::EventType::Cancel;
  e.order_id = 7;
  e.qty = 300;
  PacketBuilder packet;
  packet.start(1);
  packet.add(e);
  return packet;
}

// The bytes of a dump written like those in wire_format.hpp's comments: two hex digits per byte,
// with spaces and '|' only as separators.
std::vector<std::uint8_t> bytes_of(const std::string& dump) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < dump.size(); ++i) {
    if (dump[i] == ' ' || dump[i] == '|') continue;
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(dump.substr(i, 2), nullptr, 16)));
    ++i;
  }
  return bytes;
}

// A multicast group and port from the pid, so parallel test runs never hear each other, and a
// port of its own per test.
sockaddr_in test_group(unsigned port_offset) {
  const auto pid = static_cast<unsigned>(getpid());
  const std::string group_text =
      "239.254." + std::to_string((pid >> 8) & 255) + "." + std::to_string(pid & 255);
  sockaddr_in group{};
  make_multicast_address(group_text, static_cast<std::uint16_t>(20000 + pid % 10000 + port_offset),
                         group);
  return group;
}

// The next datagram's size, waiting up to kReceiveTimeoutNs; 0 if none came.
std::size_t receive_next(MulticastReceiver& receiver, MulticastReceiver::DatagramBuffer& datagram) {
  const std::uint64_t deadline = steady_clock_ns() + kReceiveTimeoutNs;
  while (steady_clock_ns() < deadline) {
    const std::size_t bytes = receiver.try_receive(datagram);
    if (bytes != 0) return bytes;
    usleep(100);
  }
  return 0;
}

// The four message types with a different value in every byte of every 16-, 32- and 64-bit field,
// except the cancel, which is the example in PacketBuilder::add's comment.
const book::OrderEvent kEveryMessageType[] = {
    {.order_id = 42,
     .exchange_time_ns = 34'200'000'000'250,
     .qty = 300,
     .instrument_id = 3,
     .type = book::EventType::Cancel},
    {.order_id = 0x2122232425262728,
     .exchange_time_ns = 0x1112131415161718,
     .price = 0x31323334,
     .qty = 0x3a3b3c3d,
     .instrument_id = 0x0102,
     .type = book::EventType::Add,
     .side = book::Side::Ask,
     .symbol = {'A', 'B', 'C', 'D', ' ', ' ', ' ', ' '}},
    {.order_id = 0x6162636465666768,
     .exchange_time_ns = 0x5152535455565758,
     .qty = 0x71727374,
     .instrument_id = 0x0506,
     .type = book::EventType::Execute},
    {.order_id = 0x9192939495969798,
     .new_order_id = 0xa1a2a3a4a5a6a7a8,
     .exchange_time_ns = 0x8182838485868788,
     .price = -0x0102030f,
     .qty = 0xb1b2b3b4,
     .instrument_id = 0x0708,
     .type = book::EventType::Replace},
};

}  // namespace

// Encode then decode gives back every generated event exactly.
TEST(generated_events_survive_encode_and_decode) {
  const std::vector<book::OrderEvent> events = generated(20'000);
  std::size_t checked = 0;
  std::size_t mismatched = 0;
  std::size_t per_type[5] = {};

  for (std::size_t i = 0; i < events.size(); i += kMaxMessagesPerPacket) {
    PacketBuilder packet;
    packet.start(i + 1);
    for (std::size_t k = i; k < events.size() && k < i + kMaxMessagesPerPacket; ++k) {
      packet.add(events[k]);
    }

    DecodedPacket got;
    REQUIRE(decode_packet(packet.data(), packet.size(), got));
    CHECK(got.first_seq == i + 1 && got.message_count == packet.message_count() &&
          got.event_count == got.message_count);
    for (std::uint32_t k = 0; k < got.event_count; ++k) {
      if (std::memcmp(&got.events[k], &events[i + k], sizeof(book::OrderEvent)) != 0) ++mismatched;
      ++per_type[static_cast<int>(got.events[k].type)];
      ++checked;
    }
  }

  CHECK(checked == events.size() && mismatched == 0);
  CHECK(per_type[1] > 0 && per_type[2] > 0 && per_type[3] > 0 && per_type[4] > 0);
}

// Every field of every message type, and of the header, against bytes written by hand: a byte
// swap missing in both directions, or struct padding, shows here but not in a round trip.
TEST(wire_bytes_are_big_endian_and_packed) {
  PacketBuilder packet;
  packet.start(1);
  for (const book::OrderEvent& event : kEveryMessageType)
    packet.add(event);
  const std::vector<std::uint8_t> expected = bytes_of(
      "00 00 00 00 00 00 00 01 | 00 00 00 04 | 00 00 00 00 |"  // first_seq, count, end_of_stream
      "00 17 | 58 | 00 03 | 00 00 1f 1a ce d9 f0 fa | 00 00 00 00 00 00 00 2a | 00 00 01 2c |"
      "00 24 | 41 | 01 02 | 11 12 13 14 15 16 17 18 | 21 22 23 24 25 26 27 28 | 01 |"
      "31 32 33 34 | 3a 3b 3c 3d | 41 42 43 44 20 20 20 20 |"  // price, qty, symbol
      "00 17 | 45 | 05 06 | 51 52 53 54 55 56 57 58 | 61 62 63 64 65 66 67 68 | 71 72 73 74 |"
      "00 23 | 55 | 07 08 | 81 82 83 84 85 86 87 88 | 91 92 93 94 95 96 97 98 |"
      "a1 a2 a3 a4 a5 a6 a7 a8 | fe fd fc f1 | b1 b2 b3 b4");  // new order id, price, qty
  REQUIRE(packet.size() == expected.size());
  CHECK(std::memcmp(packet.data(), expected.data(), expected.size()) == 0);
}

// Short, oversized or inconsistent packets are refused whole, never half applied.
TEST(malformed_packets_are_refused) {
  const PacketBuilder packet = one_cancel_packet();
  std::uint8_t b[kMaxPacketBytes + 1];
  std::memcpy(b, packet.data(), packet.size());
  DecodedPacket got;

  CHECK(decode_packet(b, packet.size(), got));
  CHECK(!decode_packet(b, sizeof(PacketHeader) - 1, got));  // shorter than its header
  CHECK(!decode_packet(b, packet.size() - 1, got));         // the message runs past the end
  b[packet.size()] = 0;
  CHECK(!decode_packet(b, packet.size() + 1, got));         // a byte left over

  b[11] = kMaxMessagesPerPacket + 1;  // the count's last byte
  CHECK(!decode_packet(b, packet.size(), got));
  b[11] = 1;

  b[18] = 'A';  // an add must be 36 B, not 23
  CHECK(!decode_packet(b, packet.size(), got));
}

// An unknown message type is skipped and the rest of the packet still decodes.
TEST(unknown_message_type_is_skipped) {
  const PacketBuilder packet = one_cancel_packet();
  std::uint8_t b[kMaxPacketBytes];
  std::memcpy(b, packet.data(), packet.size());
  b[18] = 'Z';

  DecodedPacket got;
  CHECK(decode_packet(b, packet.size(), got));
  CHECK(got.message_count == 1 && got.event_count == 0 && got.skipped_unknown_messages == 1);
}

// The end-of-stream packet decodes as such.
TEST(end_of_stream_packet) {
  PacketBuilder end;
  end.start_end_of_stream(51);
  DecodedPacket got;
  CHECK(end.size() == sizeof(PacketHeader) && end.data()[15] == 1);  // the flag is big-endian too
  CHECK(decode_packet(end.data(), end.size(), got));
  CHECK(got.end_of_stream && got.first_seq == 51 && got.message_count == 0);
}

// Only multicast group addresses pass the address check.
TEST(multicast_address_check) {
  sockaddr_in address{};
  CHECK(make_multicast_address("224.0.0.0", 1, address));
  CHECK(make_multicast_address("239.255.255.255", 1, address));
  CHECK(!make_multicast_address("223.255.255.255", 1, address));
  CHECK(!make_multicast_address("240.0.0.0", 1, address));
  CHECK(!make_multicast_address("127.0.0.1", 1, address));
  CHECK(!make_multicast_address("239.1", 1, address));
}

// Each packet seq is classified correctly: a gap here is what raises kInstrumentSuspect.
TEST(sequence_tracking) {
  SequenceGapTracker tracker;
  CHECK(tracker.check_packet(1, 10) == SequenceGapTracker::kInOrder);
  CHECK(tracker.check_packet(11, 10) == SequenceGapTracker::kInOrder);
  CHECK(tracker.check_packet(1, 10) == SequenceGapTracker::kDuplicate);
  CHECK(tracker.gap_count() == 0);
  CHECK(tracker.check_packet(41, 10) == SequenceGapTracker::kGap);  // events 21 to 40 are missing
  CHECK(tracker.gap_count() == 1);
  CHECK(tracker.missing_message_count() == 20);
  CHECK(tracker.check_packet(51, 0) == SequenceGapTracker::kInOrder);  // the end of stream, count 0
  CHECK(tracker.check_packet(51, 0) == SequenceGapTracker::kInOrder);  // and a copy of it
  CHECK(tracker.gap_count() == 1);  // still: a gap is never cleared

  SequenceGapTracker late;  // joins after the first 500 events
  CHECK(late.check_packet(501, 10) == SequenceGapTracker::kGap);
  CHECK(late.missing_message_count() == 500);
  CHECK(late.gap_count() == 1);
}

// Real sockets: every packet sent to the group arrives intact and in order.
TEST(loopback_round_trip) {
  constexpr std::uint32_t kPacketCount = 22;

  const sockaddr_in group = test_group(0);
  MulticastReceiver receiver;
  REQUIRE(receiver.open(group));
  MulticastSender sender;
  REQUIRE(sender.open(group));

  // Every count from 0 to 10, twice, with generated events.
  const std::vector<book::OrderEvent> events = generated(kPacketCount * kMaxMessagesPerPacket);
  PacketBuilder sent[kPacketCount];
  std::uint64_t seq = 1;
  for (std::uint32_t p = 0; p < kPacketCount; ++p) {
    sent[p].start(seq);
    for (std::uint32_t e = 0; e < p % (kMaxMessagesPerPacket + 1); ++e) {
      sent[p].add(events[seq - 1]);
      ++seq;
    }
    REQUIRE(sender.send(sent[p].data(), sent[p].size()));
  }

  std::uint32_t received = 0;
  std::uint32_t mismatched = 0;
  MulticastReceiver::DatagramBuffer datagram;
  for (; received < kPacketCount; ++received) {
    const std::size_t bytes = receive_next(receiver, datagram);
    if (bytes == 0) break;
    const PacketBuilder& want = sent[received];
    DecodedPacket got;
    const bool same = bytes == want.size() && std::memcmp(datagram.data(), want.data(), bytes) == 0;
    if (!same || !decode_packet(datagram.data(), bytes, got)) ++mismatched;
  }

  CHECK(received == kPacketCount);
  CHECK(mismatched == 0);

  // A datagram larger than any packet PacketBuilder makes arrives whole, so the decoder judges it
  // in full: a buffer of the largest packet's size would cut it, and a cut one can decode as a
  // valid shorter packet.
  const std::vector<std::uint8_t> oversized(1000, 0xee);
  REQUIRE(sender.send(oversized.data(), oversized.size()));
  CHECK(receive_next(receiver, datagram) == oversized.size());
}
