#pragma once

// The exchange feed's transport: wire_format.hpp packets over UDP multicast on the loopback
// interface (127.0.0.1), the way exchanges publish market data.
// - MulticastSender is used by sim/exchange_sim.cpp, MulticastReceiver by src/feed_handler.cpp
//   and tests/udp_feed_test.cpp. Data flow: exchange -> [this socket] -> feed handler.
// - Multicast: one send reaches every socket that joined the group address.
// - Nothing is acknowledged: a lost packet shows up only as a gap in the sequence numbers
//   (wire::SequenceGapTracker).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mdbus {

// IPv4 multicast is 224.0.0.0 to 239.255.255.255, so the first byte is 224 to 239.
constexpr std::uint32_t kFirstMulticastByte = 224;
constexpr std::uint32_t kLastMulticastByte = 239;

// Fills out with group:port. False if group is not a dotted IPv4 address or not multicast.
inline bool make_multicast_address(const std::string& group, std::uint16_t port, sockaddr_in& out) {
  out = sockaddr_in{};
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (inet_pton(AF_INET, group.c_str(), &out.sin_addr) != 1) return false;
  const std::uint32_t first_byte = ntohl(out.sin_addr.s_addr) >> 24;  // top byte of the four
  return first_byte >= kFirstMulticastByte && first_byte <= kLastMulticastByte;
}

// Shared by both sockets' open() and destructor.

inline void close_socket(int& socket_fd) {
  if (socket_fd >= 0) ::close(socket_fd);
  socket_fd = -1;
}

// Prints the failed step with errno's text, closes the socket and returns false, so open() can
// end with `return close_after_setup_error(socket_fd, "bind");`.
inline bool close_after_setup_error(int& socket_fd, const char* step) {
  std::perror(step);
  close_socket(socket_fd);
  return false;
}

// Sends to one group, on this machine only.
class MulticastSender {
 private:
  int socket_fd = -1;

 public:
  MulticastSender() = default;
  MulticastSender(const MulticastSender&) = delete;
  MulticastSender& operator=(const MulticastSender&) = delete;

  ~MulticastSender() {
    close_socket(socket_fd);
  }

  // Creates the socket and connects it to group. False (after printing why) on any failure.
  // - IP_MULTICAST_IF loopback: packets go out through 127.0.0.1, not the default network
  //   interface.
  // - IP_MULTICAST_LOOP 1: the sending host's own receivers get a copy; they are the only ones.
  // - IP_MULTICAST_TTL 0: no packet leaves the machine, even if a route would allow it.
  // - connect(): fixes the destination, so send() needs no address.
  bool open(const sockaddr_in& group) {
    socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) return close_after_setup_error(socket_fd, "socket");
    const in_addr loopback_interface{htonl(INADDR_LOOPBACK)};
    const unsigned char deliver_to_this_host = 1;
    const unsigned char time_to_live = 0;

    if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &loopback_interface,
                   sizeof loopback_interface) != 0)
      return close_after_setup_error(socket_fd, "IP_MULTICAST_IF");
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &deliver_to_this_host,
                   sizeof deliver_to_this_host) != 0)
      return close_after_setup_error(socket_fd, "IP_MULTICAST_LOOP");
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &time_to_live,
                   sizeof time_to_live) != 0)
      return close_after_setup_error(socket_fd, "IP_MULTICAST_TTL");
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&group), sizeof group) != 0)
      return close_after_setup_error(socket_fd, "connect");
    return true;
  }

  // One datagram. False unless the kernel took all size bytes.
  bool send(const std::uint8_t* data, std::size_t size) {
    return ::send(socket_fd, data, size, 0) == static_cast<ssize_t>(size);
  }
};

// Receives one group's datagrams, without ever blocking.
class MulticastReceiver {
 private:
  int socket_fd = -1;

 public:
  // About 10,500 full packets, about half a second at 20,000 packets/s.
  static constexpr int kReceiveBufferBytes = 4 * 1024 * 1024;

  MulticastReceiver() = default;
  MulticastReceiver(const MulticastReceiver&) = delete;
  MulticastReceiver& operator=(const MulticastReceiver&) = delete;

  ~MulticastReceiver() {
    close_socket(socket_fd);
  }

  // Creates the socket, binds it to group and joins the group. False (after printing why) on any
  // failure.
  // - SO_REUSEADDR: several receivers (a feed handler and a test, say) can share group and port.
  // - SO_RCVBUF kReceiveBufferBytes: room for a burst while the feed handler is busy applying.
  // - bind() to the group address, not INADDR_ANY: the kernel then hands this socket only that
  //   group's datagrams, not those of another group on the same port.
  // - IP_ADD_MEMBERSHIP on the loopback interface: the join that makes the kernel deliver the
  //   group here. Once open() returns true, the socket is listening.
  bool open(const sockaddr_in& group) {
    socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) return close_after_setup_error(socket_fd, "socket");
    const int allow_shared_port = 1;
    const int receive_buffer_bytes = kReceiveBufferBytes;
    ip_mreq membership{};
    membership.imr_multiaddr = group.sin_addr;
    membership.imr_interface.s_addr = htonl(INADDR_LOOPBACK);

    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &allow_shared_port,
                   sizeof allow_shared_port) != 0)
      return close_after_setup_error(socket_fd, "SO_REUSEADDR");
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
                   sizeof receive_buffer_bytes) != 0)
      return close_after_setup_error(socket_fd, "SO_RCVBUF");
    if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&group), sizeof group) != 0)
      return close_after_setup_error(socket_fd, "bind");
    if (setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership) != 0)
      return close_after_setup_error(socket_fd, "IP_ADD_MEMBERSHIP");
    return true;
  }

  // Copies one queued datagram into out. Returns the datagram's size, or 0 when nothing is
  // queued or the call failed.
  // - A socket error counts as "nothing queued": the caller polls again, and a lost packet is
  //   caught later as a sequence gap.
  // - A datagram longer than capacity is cut to capacity, so pass one byte more than the largest
  //   packet: an oversized datagram then still fails to decode.
  std::size_t try_receive(std::uint8_t* out, std::size_t capacity) {
    const ssize_t received = ::recv(socket_fd, out, capacity, MSG_DONTWAIT);
    if (received <= 0) return 0;
    return static_cast<std::size_t>(received);
  }
};

}  // namespace mdbus
