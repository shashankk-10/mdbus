#pragma once

// BusReader: open and validate a named bus read-only.
// - Used by Consumer::attach (consumer.hpp) and by benchmark readers that poll a RingReader
//   directly. It finds and checks the segment; reading messages is the RingReader's job.
// - The segment is mapped PROT_READ, so a buggy reader faults instead of corrupting the bus for
//   everyone. Readers write no shared memory at all, so the writer never learns how many there
//   are, and a dead reader costs it nothing.
// - Data flow: BusWriter creates the segment -> BusReader maps it -> Consumer reads the ring.

#include <unistd.h>

#include <cstdint>
#include <string>

#include "mdbus/bus_paths.hpp"
#include "mdbus/clock.hpp"
#include "mdbus/segment_format.hpp"
#include "mdbus/shared_memory.hpp"

namespace mdbus {

// The reader's side of one named bus: its paths, its read-only mapping and the typed pointers
// into it. Not copyable: it owns the mapping.
template <class Layout = BusLayout<>>
class BusReader {
 public:
  static constexpr unsigned kOpenRetrySleepMicroseconds = 500;  // between attempts while waiting

 private:
  std::string bus_name;
  BusPaths bus_paths;
  SharedMemoryMapping segment;
  SegmentPointers<Layout> segment_pointers{};

 public:
  BusReader() = default;
  BusReader(const BusReader&) = delete;
  BusReader& operator=(const BusReader&) = delete;
  ~BusReader() {
    close();
  }

  // Maps bus `name` read-only and checks it was built by a writer with this reader's layout.
  // - With timeout_ns > 0 it retries, every 500 us, the two results that clear up on their own:
  //   NoSuchBus (no writer yet) and NotReady (a writer mid-init, or a segment a newer writer is
  //   replacing). Any other result returns at once. UINT64_MAX waits forever.
  // Example (a writer created bus "demo" with 1024 instruments):
  //   open("demo")                    -> Ok; pointers().instrument_count 1024,
  //                                      paths().segment_name "/mdb.demo.d"
  //   the same, built with 128 slots  -> LayoutMismatch
  //   open("nosuch")                  -> NoSuchBus at once
  //   open("nosuch", 5'000'000)       -> NoSuchBus after about 5.5 ms
  //   open("my.bus")                  -> BadName ('.' is not allowed in a name)
  Status open(const std::string& name, std::uint64_t timeout_ns = 0) {
    const std::uint64_t now = steady_clock_ns();
    std::uint64_t deadline = UINT64_MAX;
    if (timeout_ns < UINT64_MAX - now) deadline = now + timeout_ns;
    while (true) {
      const Status status = try_open(name);
      const bool transient = status == Status::NoSuchBus || status == Status::NotReady;
      if (!transient || steady_clock_ns() >= deadline) return status;
      usleep(kOpenRetrySleepMicroseconds);
    }
  }

  void close() {
    segment_pointers = SegmentPointers<Layout>();
    segment.unmap();
  }

  const SegmentPointers<Layout>& pointers() const {
    return segment_pointers;
  }

  const BusPaths& paths() const {
    return bus_paths;
  }

  const std::string& name() const {
    return bus_name;
  }

 private:
  // One attempt, no waiting. Leaves nothing mapped unless it returns Ok.
  Status try_open(const std::string& name) {
    // 1. Name -> paths.
    close();
    if (!make_bus_paths(name, bus_paths)) return Status::BadName;
    bus_name = name;

    // 2. Map it read-only, at least the control block, and check the identity: ready marker,
    //    layout hash, instrument count and size.
    Status status =
        segment.open(bus_paths.segment_name, Access::ReadOnly, sizeof(ControlBlock<StdAtomics>));
    if (status != Status::Ok) return unmap_and_return(status);
    const auto& control = *static_cast<const ControlBlock<StdAtomics>*>(segment.address());
    status = SegmentFormat<Layout>::validate(control, segment.size());
    if (status != Status::Ok) return unmap_and_return(status);

    // 3. A segment a newer writer has replaced is on its way out: wait for the new one.
    if (load_writer_state(control) == WriterState::Replaced) {
      return unmap_and_return(Status::NotReady);
    }

    // 4. Point into it. Relaxed is enough: validate() read the ready marker with acquire, and the
    //    writer stores the marker last.
    const std::uint64_t instrument_count =
        StdAtomics::load_relaxed(control.identity.instrument_count);
    segment_pointers = SegmentFormat<Layout>::pointers_into(
        segment.address(), static_cast<std::uint32_t>(instrument_count));
    return Status::Ok;
  }

  Status unmap_and_return(Status status) {
    segment.unmap();
    return status;
  }
};

}  // namespace mdbus
