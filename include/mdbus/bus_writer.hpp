#pragma once

// BusWriter: the one object a writer process opens to own a named bus.
// - Holds the writer lock (writer_liveness.hpp), the shm segment (shared_memory.hpp) and the
//   Publisher that writes into it (publisher.hpp).
// - Used by src/feed_handler.cpp, failure_test and baseline/bus_variants.hpp. Readers attach
//   to the same name with BusReader.
// - flock: an advisory lock on an open file, which the kernel drops when the holder dies.

#include <sys/mman.h>

#include <cstdint>
#include <string>

#include "mdbus/bus_paths.hpp"
#include "mdbus/publisher.hpp"
#include "mdbus/shared_memory.hpp"
#include "mdbus/writer_liveness.hpp"

namespace mdbus {

// Owns the writer side of a named bus, from open() to close().
// - open() takes the flock, retires any old segment (marks it Replaced so its readers re-attach,
//   then unlinks it), creates and fills a new one, and stores the ready marker last.
// - A restarted writer never resumes a dead writer's ring. Reusing it would mean trusting
//   whatever half-written slot the dead one left; a fresh segment costs one re-attach per reader.
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
  // - Returns BadName, BadArgument, AnotherWriterRunning (lock still held after 10 ms), the
  //   Status of a failed segment create, or Ok.
  // Example (user id 501, a crashed writer left "/mdb.demo.d" behind):
  //   open("demo")    -> Ok: locks /var/tmp/mdbus-501/demo.lock, marks the old segment Replaced
  //                      and unlinks it, creates a new "/mdb.demo.d" for 1024 instruments,
  //                      WriterState Running
  //   open("demo") while another writer runs  -> AnotherWriterRunning, nothing touched
  //   open("my.bus")  -> BadName;  open("demo", 0) -> BadArgument
  Status open(const std::string& name, std::uint32_t instrument_count = kDefaultInstrumentCount) {
    close();
    if (!make_bus_paths(name, bus_paths)) return Status::BadName;
    if (instrument_count == 0 || instrument_count > kMaxInstrumentCount)
      return Status::BadArgument;

    // 1. Lock, then retire whatever segment a previous writer left.
    const Status locked = writer_lock.acquire_exclusive(bus_paths, kWriterLockTimeoutNs);
    if (locked != Status::Ok) return locked;
    retire_previous_segment();

    // 2. Create the new segment. On failure, unlink any half-made object and drop the lock.
    const Status status = segment.create(bus_paths.segment_name,
                                         SegmentFormat<Layout>::segment_bytes(instrument_count));
    if (status != Status::Ok) {
      shm_unlink(bus_paths.segment_name.c_str());
      release_segment_and_lock();
      return status;
    }

    // 3. Fill it (construct() stores the ready marker last), then announce Running.
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
  // Marks the bus's current segment Replaced, if it is a complete one, and unlinks the name.
  // - Readers still mapped to it see Replaced and re-attach to the new segment by name.
  // - A segment without the ready marker was never finished, so no reader is attached to it.
  void retire_previous_segment() {
    SharedMemoryMapping previous_segment;
    const Status opened = previous_segment.open(bus_paths.segment_name, Access::ReadWrite,
                                                sizeof(ControlBlock<StdAtomics>));
    if (opened == Status::Ok) {
      auto* control = static_cast<ControlBlock<StdAtomics>*>(previous_segment.address());
      if (StdAtomics::load_acquire(control->identity.ready_marker) == kSegmentReadyMarker)
        store_writer_state(*control, WriterState::Replaced);
    }
    shm_unlink(bus_paths.segment_name.c_str());
  }

  // Drops the publisher, the mapping and, last, the flock.
  void release_segment_and_lock() {
    bus_publisher = Publisher<Layout>();
    segment_pointers = SegmentPointers<Layout>();
    segment.unmap();
    writer_lock.close();
  }
};

}  // namespace mdbus
