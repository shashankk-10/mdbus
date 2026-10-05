#pragma once

#include <pthread.h>

#include <cstring>
#include <new>

#include "mdbus/ring.hpp"
#include "mdbus/segment_format.hpp"

// The ring behind the Mutex variant: the production slots under one pthread mutex.
// - Used only by baseline/bus_variants.hpp (VariantBus<..., MutexProtocol>) inside bus_bench.
// - Same slot layout and payload words as the stamp ring, but Mutex -> Base measures more than
//   the synchronisation. Every reader poll writes the lock, so readers are coupled to the
//   writer. There is no snapshot table: Base's paced writer also writes a snapshot every 16
//   messages and its slow reader reads one every 64, which Mutex skips (that favours Mutex).
// - With the slow reader present (the e2e comparison), an E-core process at BACKGROUND QoS takes
//   the ring's lock for every poll and copy. The writer waits whenever that reader holds it,
//   longest when the reader is descheduled while holding it: about three in four of the writer's
//   publishes ran late (writer_late: 76%, results/runs.csv).

namespace mdbus::baseline {

// One writer and any number of readers sharing a ring through a process-shared mutex.
// - Every reader poll takes the lock, so the lock line bounces between the writer's core and
//   every reader's core. That traffic is the cost the stamp protocol avoids.
// - FIRSTFIT: the thread that releases the lock may take it straight back, instead of handing
//   it to a waiter (FAIRSHARE, the macOS default). It is the faster policy for a writer that
//   locks once per message, so the stamp ring is not compared against a weakened baseline.
// - Robust mutex = one that tells the next locker its holder died. macOS has none, so a holder
//   that dies inside the lock blocks every other process for good (a dead stamp-ring writer
//   leaves one odd slot). Fine for a benchmark, not for the bus.
template <class Layout = BusLayout<>>
class MutexRing {
 public:
  using PayloadWords = typename Layout::PayloadWords;
  static constexpr std::uint64_t kSlotIndexMask = Layout::kSlotCount - 1;

  // The stamp ring's slot layout with plain words. The stamp word is never used; it keeps the
  // payload at the same offset in the same cache line.
  struct alignas(Layout::kSlotBytes) PlainSlot {
    std::uint64_t stamp;
    std::uint64_t payload_words[Layout::kPayloadWords];
  };
  static_assert(sizeof(PlainSlot) ==
                sizeof(Slot<StdAtomics, Layout::kPayloadWords, Layout::kSlotBytes>));

  // The lock is the one line every process writes, so it gets a 128 B line to itself, and so
  // does the head. Keep these alignas as they are: the Mutex rows in results/ depend on them.
  struct alignas(kCacheLineBytes) Lock {
    pthread_mutex_t mutex;
  };
  struct alignas(kCacheLineBytes) Head {
    std::uint64_t next_seq;  // the next seq to publish; read and written only under the lock
  };
  static_assert(sizeof(pthread_mutex_t) <= kCacheLineBytes && sizeof(Lock) == kCacheLineBytes);

  // The whole shared segment, placed at the start of the mapping.
  struct Shared {
    Lock lock;
    Head head;
    PlainSlot slots[Layout::kSlotCount];
  };
  static constexpr std::size_t kSize = round_up_to_multiple(sizeof(Shared), kPageSize);

 private:
  Shared* shared = nullptr;

 public:
  // Writer side: builds Shared in a new mapping of kSize bytes and initialises the mutex.
  // Value-initialised, so the head starts at 0.
  bool create(void* segment_start) {
    shared = new (segment_start) Shared();
    return init_shared_mutex(&shared->lock.mutex) == 0;
  }

  // Reader side. The launcher starts readers only after the writer reports ready, so the mutex
  // is initialised by then.
  void attach(void* segment_start) {
    shared = static_cast<Shared*>(segment_start);
  }

  // Copies the words into the next slot and bumps the head, all under the lock.
  void publish(const PayloadWords& words) {
    pthread_mutex_lock(&shared->lock.mutex);
    const std::uint64_t seq = shared->head.next_seq;
    shared->head.next_seq = seq + 1;
    std::memcpy(shared->slots[seq & kSlotIndexMask].payload_words, words.data(), sizeof words);
    pthread_mutex_unlock(&shared->lock.mutex);
  }

  // Copies message seq out under the lock, with the same three results as RingReader::try_poll.
  // - NotWrittenYet: the head has not passed seq.
  // - Lapped: the head is more than one ring ahead, so seq's slot already holds a later message.
  TryPollResult poll(std::uint64_t seq, PayloadWords& payload) {
    pthread_mutex_lock(&shared->lock.mutex);
    const std::uint64_t next_seq_to_publish = shared->head.next_seq;
    TryPollResult result = TryPollResult::Ok;
    if (next_seq_to_publish <= seq) {
      result = TryPollResult::NotWrittenYet;
    } else if (next_seq_to_publish - seq > Layout::kSlotCount) {
      result = TryPollResult::Lapped;
    } else {
      std::memcpy(payload.data(), shared->slots[seq & kSlotIndexMask].payload_words,
                  sizeof payload);
    }
    pthread_mutex_unlock(&shared->lock.mutex);
    return result;
  }

  std::uint64_t next_seq() {  // takes the lock too
    pthread_mutex_lock(&shared->lock.mutex);
    const std::uint64_t next_seq_to_publish = shared->head.next_seq;
    pthread_mutex_unlock(&shared->lock.mutex);
    return next_seq_to_publish;
  }

 private:
  // Process-shared, FIRSTFIT. Returns 0 or the first pthread error.
  static int init_shared_mutex(pthread_mutex_t* mutex) {
    pthread_mutexattr_t attributes;
    int error = pthread_mutexattr_init(&attributes);
    if (error != 0) return error;
    error = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (error == 0) {
      error = pthread_mutexattr_setpolicy_np(&attributes, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);
    }
    if (error == 0) error = pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return error;
  }
};

}  // namespace mdbus::baseline
