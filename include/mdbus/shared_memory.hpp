#pragma once

// SharedMemoryMapping: one mmap of named or anonymous shared memory, pre-faulted and locked.
// - BusWriter creates a bus segment with create(), BusReader maps it with open(); in-process
//   tests and benches use create_anonymous() and skip the shm name.
// - Knows nothing about what is inside the memory: segment_format.hpp lays the bus out in it,
//   and bus_paths.hpp picks the name.

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "mdbus/constants.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// Readers map ReadOnly, so a buggy reader faults instead of corrupting the bus for everyone.
enum class Access : std::uint8_t { ReadOnly, ReadWrite };

// Move-only owner of one mapping; unmaps it on destruction.
// - Every page is touched and locked in RAM when it is mapped, so neither a first-touch fault
//   nor a fault on a compressed page lands on a publish or a poll.
// - Why mlock on top of the touch: the touch only fixes the first fault. Under memory pressure
//   macOS compresses idle pages, and the next access faults again.
class SharedMemoryMapping {
 private:
  static constexpr mode_t kOwnerOnlyFileMode = 0600;  // read and write for this user only

  void* mapped_address = nullptr;
  std::size_t mapped_bytes = 0;

 public:
  SharedMemoryMapping() = default;
  SharedMemoryMapping(SharedMemoryMapping&& other) noexcept {
    *this = std::move(other);
  }

  SharedMemoryMapping& operator=(SharedMemoryMapping&& other) noexcept {
    if (this != &other) {
      unmap();
      mapped_address = other.mapped_address;
      mapped_bytes = other.mapped_bytes;
      other.mapped_address = nullptr;
      other.mapped_bytes = 0;
    }
    return *this;
  }

  ~SharedMemoryMapping() {
    unmap();
  }

  // Creates the shm object `name` (it must not exist yet), sizes it and maps it read-write.
  // - AlreadyExists if the name is taken; SystemCallFailed for any other error.
  // - macOS allows exactly one ftruncate on a shm object, so size_bytes is final. If the
  //   ftruncate fails, the object is unlinked again rather than left at size 0.
  Status create(const std::string& name, std::size_t size_bytes) {
    unmap();
    const int shm_fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, kOwnerOnlyFileMode);
    if (shm_fd < 0) {
      if (errno == EEXIST) return Status::AlreadyExists;
      return Status::SystemCallFailed;
    }
    if (ftruncate(shm_fd, static_cast<off_t>(size_bytes)) == 0) {
      return map(shm_fd, size_bytes, Access::ReadWrite);
    }
    ::close(shm_fd);
    shm_unlink(name.c_str());
    return Status::SystemCallFailed;
  }

  // Maps an existing shm object, all of it, if it is at least min_size_bytes long.
  // - NoSuchBus: no object has that name.
  // - NotReady: the object has size 0, so its creator has not run ftruncate yet.
  // - NotABusSegment: it is non-empty but shorter than min_size_bytes.
  // - The size is read with fstat before mmap: mapping past the end of a short object would
  //   not fail here but raise SIGBUS on the first touch.
  Status open(const std::string& name, Access access, std::size_t min_size_bytes) {
    unmap();
    int open_flags = O_RDONLY;
    if (access == Access::ReadWrite) open_flags = O_RDWR;
    const int shm_fd = shm_open(name.c_str(), open_flags);
    if (shm_fd < 0) {
      if (errno == ENOENT) return Status::NoSuchBus;
      return Status::SystemCallFailed;
    }

    struct stat info{};
    std::size_t object_bytes = 0;
    if (fstat(shm_fd, &info) == 0) object_bytes = static_cast<std::size_t>(info.st_size);
    if (object_bytes != 0 && object_bytes >= min_size_bytes) {
      return map(shm_fd, object_bytes, access);
    }
    ::close(shm_fd);
    if (object_bytes == 0) return Status::NotReady;
    return Status::NotABusSegment;
  }

  // Anonymous shared memory for one process (tests, benches); aborts if mmap fails.
  static SharedMemoryMapping create_anonymous(std::size_t size_bytes) {
    SharedMemoryMapping mapping;
    check_or_abort(mapping.map(-1, size_bytes, Access::ReadWrite) == Status::Ok,
                   "mmap of anonymous shared memory");
    return mapping;
  }

  void* address() const {
    return mapped_address;
  }

  std::size_t size() const {
    return mapped_bytes;
  }

  bool is_mapped() const {
    return mapped_address != nullptr;
  }

  void unmap() {
    if (mapped_address != nullptr) munmap(mapped_address, mapped_bytes);
    mapped_address = nullptr;
    mapped_bytes = 0;
  }

 private:
  // Maps shm_fd (or anonymous memory when shm_fd is -1), then faults in and locks every page.
  Status map(int shm_fd, std::size_t size_bytes, Access access) {
    // 1. Map. The fd is closed right away: the mapping keeps the object alive.
    const bool writable = access == Access::ReadWrite;
    int protection = PROT_READ;
    if (writable) protection = PROT_READ | PROT_WRITE;
    int map_flags = MAP_SHARED;
    if (shm_fd < 0) map_flags = MAP_SHARED | MAP_ANON;

    void* new_address = mmap(nullptr, size_bytes, protection, map_flags, shm_fd, 0);
    if (shm_fd >= 0) ::close(shm_fd);
    if (new_address == MAP_FAILED) return Status::SystemCallFailed;
    mapped_address = new_address;
    mapped_bytes = size_bytes;

    // 2. Touch one word per 16 KB page, so page faults happen now and not on the first publish
    //    (measured runs require zero page faults).
    //    - On a writable page an atomic add of 0 forces the write fault without changing
    //      anything another process can see; other processes may already be using the memory.
    for (std::size_t offset = 0; offset < mapped_bytes; offset += kPageSize) {
      auto* word = reinterpret_cast<std::atomic<std::uint64_t>*>(static_cast<char*>(new_address) +
                                                                  offset);
      if (writable)
        word->fetch_add(0, std::memory_order_relaxed);
      else
        word->load(std::memory_order_relaxed);
    }

    // 3. Lock the pages in RAM so macOS never compresses or pages them out. Needs no root;
    //    failure only prints a warning.
    if (mlock(new_address, size_bytes) != 0) {
      std::fprintf(stderr, "mdbus: mlock failed, continuing unwired\n");
    }
    return Status::Ok;
  }
};

}  // namespace mdbus
