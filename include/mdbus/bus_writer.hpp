#pragma once

// BusWriter: the one object a writer process opens to own a named bus.
// - Holds the writer lock (writer_liveness.hpp), the shm segment (shared_memory.hpp) and the
//   Publisher that writes into it (publisher.hpp). Readers attach to the same name with
//   BusReader.

#include <sys/mman.h>

#include <cstdint>
#include <string>

#include "mdbus/bus_paths.hpp"
#include "mdbus/publisher.hpp"
#include "mdbus/shared_memory.hpp"
#include "mdbus/writer_liveness.hpp"

namespace mdbus {

// Owns the writer side of a named bus, from open() to close().
// - open() takes the flock, unlinks any old segment of the name, creates and fills a new one,
//   and stores the ready marker last.
// - A restarted writer never resumes a dead writer's ring. Resuming would mean finding where the
//   dead one stopped and trusting or repairing the slot and snapshot it may have left half
//   written, while readers are still mapped. A fresh segment costs each reader one attach:
//   readers of the old one see the writer Down and attach again by name.
template <class Layout = BusLayout<>>
class BusWriter {
 private:
  BusPaths bus_paths;
  WriterLock writer_lock;
  SharedMemoryMapping segment;
  SegmentPointers<Layout> segment_pointers{};
  Publisher<Layout> bus_publisher;

 public:
  ~BusWriter() {
    close();
  }

  // Opens bus `name` for writing, with a snapshot table of instrument_count rows.
  // - The flock comes first: only its holder may replace a segment, because then no live
  //   writer can still be using the old one.
  // - Returns BadName, BadArgument, AnotherWriterRunning, SystemCallFailed (the lock file could
  //   not be opened), the Status of a failed segment create, or Ok.
  Status open(const std::string& name, std::uint32_t instrument_count = kDefaultInstrumentCount) {
    close();
    if (!make_bus_paths(name, bus_paths)) return Status::BadName;
    if (instrument_count == 0 || instrument_count > kMaxInstrumentCount)
      return Status::BadArgument;

    // 1. Lock, then unlink whatever segment a previous writer left. Readers still mapped to it
    //    keep their mapping until they let go.
    const Status locked = writer_lock.acquire(bus_paths);
    if (locked != Status::Ok) return locked;
    shm_unlink(bus_paths.segment_name.c_str());

    // 2. Create the new segment. On failure create() has already removed any half-made object,
    //    so only the lock is left to drop.
    const Status status = segment.create(bus_paths.segment_name,
                                         SegmentFormat<Layout>::segment_bytes(instrument_count));
    if (status != Status::Ok) {
      release_segment_and_lock();
      return status;
    }

    // 3. Fill it (construct() stores the ready marker last), then the first heartbeat.
    segment_pointers = SegmentFormat<Layout>::construct(segment.address(), instrument_count);
    bus_publisher = Publisher<Layout>(segment_pointers);
    bus_publisher.mark_running();
    return Status::Ok;
  }

  // Marks the segment exited, then releases it and, last, the flock.
  void close() {
    if (segment.is_mapped()) bus_publisher.mark_exited();
    release_segment_and_lock();
  }

  Publisher<Layout>& publisher() {
    return bus_publisher;
  }

  const SegmentPointers<Layout>& pointers() const {
    return segment_pointers;
  }

 private:
  // Drops the publisher, the mapping and, last, the flock.
  void release_segment_and_lock() {
    bus_publisher = Publisher<Layout>();
    segment_pointers = SegmentPointers<Layout>();
    segment.unmap();
    writer_lock.close();
  }
};

}  // namespace mdbus
