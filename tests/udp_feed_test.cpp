// The feed's wire format, sequence check and loopback multicast link.
// - Generated events survive encode and decode unchanged; the bytes are big-endian and packed.
// - Malformed packets are refused whole, unknown message types skipped, and only multicast
//   addresses accepted.
// - SequenceGapTracker classifies in-order, duplicate, gapped and late-join packets.
// - Packets sent to a group arrive intact and in order at a receiver on this machine.

#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "sim/order_event_generator.hpp"
#include "mdbus/clock.hpp"
#include "mdbus/feed/multicast_socket.hpp"
#include "mdbus/feed/wire_format.hpp"
#include "test_harness.hpp"

using namespace mdbus;
using namespace mdbus::wire;

namespace {

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

// The bytes on the wire, checked at fixed offsets: catches host byte order or struct padding.
TEST(wire_bytes_are_big_endian_and_packed) {
  const PacketBuilder packet = one_cancel_packet();
  const std::uint8_t* b = packet.data();
  CHECK(packet.size() == 16 + 2 + 23);  // header, length, a 23 B cancel
  CHECK(b[7] == 1 && b[0] == 0);        // seq 1, most significant byte first
  CHECK(b[16] == 0 && b[17] == 23);     // the message length
  CHECK(b[18] == 'X');                  // ITCH's cancel letter
  CHECK(b[18 + 18] == 0x07);            // order id 7: the last of its 8 bytes at offset 11
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
  CHECK(end.size() == sizeof(PacketHeader));
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
  CHECK(!tracker.has_seen_gap());
  CHECK(tracker.check_packet(41, 10) == SequenceGapTracker::kGap);  // events 21 to 40 are missing
  CHECK(tracker.gap_count() == 1);
  CHECK(tracker.missing_message_count() == 20);
  CHECK(tracker.check_packet(51, 0) == SequenceGapTracker::kInOrder);  // the end of stream, count 0
  CHECK(tracker.check_packet(51, 0) == SequenceGapTracker::kInOrder);  // and a copy of it
  CHECK(tracker.has_seen_gap());  // still: a gap is never cleared

  SequenceGapTracker late;  // joins after the first 500 events
  CHECK(late.check_packet(501, 10) == SequenceGapTracker::kGap);
  CHECK(late.missing_message_count() == 500);
  CHECK(late.has_seen_gap());
}

// Real sockets: every packet sent to the group arrives intact and in order.
TEST(loopback_round_trip) {
  constexpr std::uint32_t kPacketCount = 22;
  constexpr std::uint64_t kTimeoutNs = 2'000'000'000;

  // A group and port from the pid, so parallel test runs never hear each other.
  const auto pid = static_cast<unsigned>(getpid());
  const std::string group_text =
      "239.254." + std::to_string((pid >> 8) & 255) + "." + std::to_string(pid & 255);
  const auto port = static_cast<std::uint16_t>(20000 + pid % 10000);

  sockaddr_in group{};
  REQUIRE(make_multicast_address(group_text, port, group));
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
  const std::uint64_t deadline = steady_clock_ns() + kTimeoutNs;
  std::uint8_t datagram[kMaxPacketBytes + 1];
  while (received < kPacketCount && steady_clock_ns() < deadline) {
    const std::size_t bytes = receiver.try_receive(datagram, sizeof datagram);
    if (bytes == 0) {
      usleep(100);
      continue;
    }
    const PacketBuilder& want = sent[received];
    DecodedPacket got;
    const bool same = bytes == want.size() && std::memcmp(datagram, want.data(), bytes) == 0;
    if (!same || !decode_packet(datagram, bytes, got)) ++mismatched;
    ++received;
  }

  CHECK(received == kPacketCount);
  CHECK(mismatched == 0);
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
