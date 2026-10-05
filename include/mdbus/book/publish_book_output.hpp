#pragma once

// The step from book to bus: one event's BookOutput becomes bus slots.
// - Data flow: FeedBook::apply -> publish_book_output -> Publisher -> ring and snapshot table.
// - Used by src/feed_handler.cpp and by tests/book_through_bus_test.cpp and no_alloc_test.cpp.

#include <cstdint>

#include "mdbus/book/feed_book.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/publisher.hpp"

namespace mdbus::book {

// Publishes an InstrumentStatus for instrument_id together with its snapshot.
// - The status carries the snapshot's flags (the book's, with the caller's feed_flags ORed in).
// - reject_reason codes: 0 None, 1 Malformed, 2 PriceOutsideWindow, 3 OrderTableFull,
//   4 Duplicate. None is what the feed handler sends when it marks everything suspect.
template <class PublisherType>
void publish_instrument_status(PublisherType& publisher, std::uint16_t instrument_id,
                               const InstrumentSnapshot& snapshot, RejectReason reject_reason) {
  InstrumentStatus status{};
  status.instrument_flags = static_cast<std::uint8_t>(snapshot.instrument_flags);
  status.reject_reason = static_cast<std::uint32_t>(reject_reason);
  publisher.publish_and_update_snapshot(instrument_id, status, snapshot);
}

// Puts one event's output on the bus and returns how many slots it published (0 to 3).
// Publish order:
//   1. Trade, as a plain slot: no snapshot includes a trade, and a reader sees the fill before
//      the liquidity it took disappears.
//   2. BookDelta, with the new snapshot (slot first, then snapshot).
//   3. InstrumentStatus with the snapshot, if this event just marked the instrument
//      kInstrumentBad.
// - feed_flags: feed-level bits the book does not own (kInstrumentSuspect after an input gap),
//   ORed into every snapshot so a suspect one never passes for good.
// - latency_start_ticks: the packet's receive time; when non-zero it stamps the first slot only.
template <class PublisherType, class BookType>
unsigned publish_book_output(PublisherType& publisher, const BookType& book,
                             const BookOutput& book_output, std::uint32_t feed_flags,
                             std::uint64_t latency_start_ticks) {
  unsigned slots_published = 0;

  if (book_output.trade.qty != 0) {
    if (latency_start_ticks != 0) {
      publisher.publish_with_latency_start(book_output.instrument_id, book_output.trade,
                                           latency_start_ticks);
    } else {
      publisher.publish(book_output.instrument_id, book_output.trade);
    }
    latency_start_ticks = 0;  // only the first slot carries the stamp
    ++slots_published;
  }

  if (book_output.delta.entry_count == 0 && !book_output.became_bad) return slots_published;
  InstrumentSnapshot snapshot;
  book.fill_snapshot(book_output.instrument_id, snapshot);
  snapshot.instrument_flags |= feed_flags;

  if (book_output.delta.entry_count != 0) {
    publisher.publish_and_update_snapshot(book_output.instrument_id, book_output.delta, snapshot,
                                          latency_start_ticks);
    ++slots_published;
  }

  if (book_output.became_bad) {
    publish_instrument_status(publisher, book_output.instrument_id, snapshot,
                              book_output.reject_reason);
    ++slots_published;
  }
  return slots_published;
}

}  // namespace mdbus::book
