#pragma once

// The atomic policy: the shared word type plus the six load/store/fence operations the ring
// and snapshot protocols are written against.
// - Why a policy and not std::atomic calls inline: tests/mutants.hpp swaps in policies that each
//   weaken one of these operations wherever it is used (every release store made relaxed, a
//   fence dropped), and mutant_stress must catch each one on the hardware. ring_test and
//   snapshot_test use the same seam to land a write in the middle of a copy. The protocol code
//   is the same either way.
// - StdAtomics is the only policy production code uses. Each operation's trailing comment
//   names the arm64 instruction it compiles to with -mcpu=apple-m1.
// - Readers map the segment read-only. Both loads are plain load instructions on arm64, never
//   a compare-and-swap, so they work on that mapping.

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace mdbus {

// Shared memory holds std::atomic words. Lock-free means no hidden lock, so one word works
// between processes; 8 B size and alignment make it one plain load or store.
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && alignof(std::atomic<std::uint64_t>) == 8 &&
              std::atomic<std::uint64_t>::is_always_lock_free);
// The shared structs use offsetof, which needs standard-layout members.
static_assert(std::is_standard_layout_v<std::atomic<std::uint64_t>>);

// The production policy: each operation is one std::atomic call with the matching order.
struct StdAtomics {
  using Word = std::atomic<std::uint64_t>;

  static std::uint64_t load_acquire(const Word& word) {  // ldapr
    return word.load(std::memory_order_acquire);
  }

  static std::uint64_t load_relaxed(const Word& word) {  // ldr
    return word.load(std::memory_order_relaxed);
  }

  static void store_relaxed(Word& word, std::uint64_t value) {  // str
    word.store(value, std::memory_order_relaxed);
  }

  static void store_release(Word& word, std::uint64_t value) {  // stlr
    word.store(value, std::memory_order_release);
  }

  // dmb ishld: later loads and stores wait for earlier loads.
  static void fence_acquire() {
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  // dmb ish, a full barrier: a C++ release fence must also order earlier loads, which dmb ishst
  // does not. The ring only needs earlier stores ordered, but ishst would take inline asm outside
  // the C++ memory model; the C++ fence keeps the mutants weakening exactly the code that runs.
  // The cost difference was not measured.
  static void fence_release() {
    std::atomic_thread_fence(std::memory_order_release);
  }
};

// The 64-bit shared word of policy Atomics: every shared field is one.
template <class Atomics>
using SharedWord = typename Atomics::Word;

}  // namespace mdbus
