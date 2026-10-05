#pragma once

// FeedBook's lookup policy: which instrument an event is for.
// - Every event carries its dense instrument id, so the lookup is a bounds check.
// - The old design's symbol lookup (baseline/book_ladder.hpp, F0..F2) plugs in at the same place.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdbus/book/order_event.hpp"

namespace mdbus::book {

// Ids are 0..N-1 with no gaps, so an instrument's id is its array position.
class InstrumentLookupById {
 private:
  std::size_t instrument_count;

 public:
  explicit InstrumentLookupById(const std::vector<InstrumentInfo>& instrument_list)
      : instrument_count(instrument_list.size()) {}

  // The event's instrument id, or kNoInstrument if it is out of range.
  // - Takes the whole event, not the id, because the baseline symbol lookup needs the symbol.
  std::uint16_t find_instrument(const OrderEvent& event) const {
    if (event.instrument_id < instrument_count) return event.instrument_id;
    return kNoInstrument;
  }
};

}  // namespace mdbus::book
