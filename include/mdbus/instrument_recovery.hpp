#pragma once

// Per-instrument recovery state of one reader.
// - InstrumentRecoveryTable: for each instrument, is it stale, was on_stale already called, and
//   the first seq to deliver after the snapshot it recovered from.
// - Owned by Consumer (consumer.hpp), one per reader, in the reader's own memory. The recovery
//   walk-through that drives it is in consumer.hpp's file header.
// - Stale: this reader's copy of the instrument's book cannot be trusted (after a lap, at attach,
//   or after a bad status message) until it reads the instrument's snapshot.

#include <cstdint>
#include <vector>

namespace mdbus {

// One instrument's row, 16 B: the stale check and the seq filter for a delta read the same row.
struct InstrumentRecovery {
  bool is_stale;
  bool stale_reported;                 // on_stale already called since the last recovery
  std::uint64_t first_seq_to_deliver;  // lower seqs are already in the snapshot applied
};

// One row per instrument id, indexed directly by the id.
// - Every method takes an id below the instrument count given to resize(); none checks it.
//   Consumer drops a message with a bad id before it gets here.
class InstrumentRecoveryTable {
 private:
  std::vector<InstrumentRecovery> entries;

 public:
  // Allocates one row per instrument, every one not stale. Called at attach, never while
  // polling, so the poll loop never allocates.
  void resize(std::uint32_t instrument_count) {
    entries.assign(instrument_count, InstrumentRecovery{});
  }

  // After a lap, at attach and after a re-attach: no instrument's book can be trusted.
  void mark_all_stale() {
    for (InstrumentRecovery& entry : entries) {
      entry.is_stale = true;
    }
  }

  void mark_stale(std::uint16_t instrument_id) {
    entries[instrument_id].is_stale = true;
  }

  // True only the first time it is asked since the instrument last recovered; the caller then
  // calls on_stale, so a long stale spell produces one callback, not one per message.
  bool report_stale_once(std::uint16_t instrument_id) {
    InstrumentRecovery& entry = entries[instrument_id];
    if (entry.stale_reported) return false;
    entry.stale_reported = true;
    return true;
  }

  bool is_stale(std::uint16_t instrument_id) const {
    return entries[instrument_id].is_stale;
  }

  std::uint64_t first_seq_to_deliver(std::uint16_t instrument_id) const {
    return entries[instrument_id].first_seq_to_deliver;
  }

  // The instrument's snapshot was applied: not stale any more, stale reports re-armed, and
  // deltas below first_seq_to_deliver are skipped as already in the snapshot.
  void set_recovered(std::uint16_t instrument_id, std::uint64_t first_seq_to_deliver) {
    InstrumentRecovery& entry = entries[instrument_id];
    entry.is_stale = false;
    entry.stale_reported = false;
    entry.first_seq_to_deliver = first_seq_to_deliver;
  }
};

}  // namespace mdbus
