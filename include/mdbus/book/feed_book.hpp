#pragma once

// FeedBook: the feed handler's order book, for every instrument, one OrderEvent at a time.
// - Top levels (the shown levels): the best 6 price levels of a side (kTopLevelsPerSide), the
//   only part of the book consumers see. Per event the book returns a BookOutput: their change
//   on one side as a BookDelta (absolute quantities, at most 3 entries), plus a Trade for an
//   execution. publish_book_output.hpp puts it on the bus.
// - Price window: the 4096 prices, kMinPriceOffset..kMaxPriceOffset ticks from an instrument's
//   reference_price, at which its orders may rest. The reference never moves.
// - Price ladder (the trading term): the total qty at every price of the window, per side
//   (price_ladder.hpp). Not the "book ladder" of F0..F4 benchmark configs (baseline/).
// - An add is refused if it is malformed, outside the price window, past the order table's cap,
//   a live duplicate, or would wrap its level's 32-bit total. The instrument is then marked
//   kInstrumentBad for the rest of the run.
// - Three policies (price levels, order index, instrument lookup) and a prefetch distance plug
//   in through BookConfig, so the benchmarks can swap one at a time and the oracle test can
//   check each against the naive book (F0). The program always uses FinalBookConfig.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdbus/book/order_event.hpp"
#include "mdbus/book/order_table.hpp"
#include "mdbus/book/price_ladder.hpp"
#include "mdbus/book/top_levels.hpp"
#include "mdbus/constants.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/snapshot_table.hpp"
#include "mdbus/status.hpp"

namespace mdbus::book {

// Lookup policy: which instrument an event is for.
// - Every event carries its dense instrument id, so the lookup is a bounds check.
// - The naive design's symbol lookup (baseline/book_ladder.hpp, F0..F2) plugs in at the same place.
// - Ids are 0..N-1 with no gaps, so an instrument's id is its array position.
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

// The book's policies, one per sub-problem, and its prefetch distance: the config the feed
// handler runs. baseline/book_ladder.hpp derives F0..F3 from it and tabulates the steps.
struct FinalBookConfig {
  using LevelPolicy = PriceLadder;
  using OrderIndexPolicy = OrderTable;
  using LookupPolicy = InstrumentLookupById;
  // While applying event i of a batch, prefetch the order-table entry of event i + 4.
  // - Measured: -37% per event at 1 M live orders, a table far bigger than L2 (2^21 entries x
  //   24 B = 48 MiB; 100.0 -> 62.8 ns), and -3.0% at the default 16 K, inside L2 (6/6 pairs,
  //   just under the 3% threshold: unresolved). 4 was picked, not swept.
  // - The feed handler's batch is one packet, at most 10 events, so events 0..3 of each packet
  //   are never prefetched (feed_bench's batches are 1365 events).
  static constexpr std::size_t kPrefetchEventsAhead = 4;
};

// Why an add was refused. InstrumentStatus::reject_reason carries the value (0..5).
enum class RejectReason : std::uint8_t {
  None,
  Malformed,
  PriceOutsideWindow,
  OrderTableFull,
  Duplicate,
  LevelTotalOverflow
};

// One event's result.
// - delta: the change to instrument_id's shown levels; empty when delta.entry_count is 0.
// - became_bad: this event's refused add is the one that marked the instrument kInstrumentBad.
// - trade: an execution's fill; trade.qty is 0 for every other event.
// - Plain bytes with no padding, so tests compare outputs with memcmp.
struct BookOutput {
  BookDelta delta{};
  std::uint16_t instrument_id = kNoInstrument;
  bool became_bad = false;
  RejectReason reject_reason = RejectReason::None;
  Trade trade{};
};
static_assert(sizeof(BookOutput) == sizeof(BookDelta) + sizeof(std::uint16_t) + sizeof(bool) +
                                        sizeof(RejectReason) + sizeof(Trade),
              "the fields' sizes sum to the struct's: no padding for memcmp to see");

// The book's own tally since it started.
struct BookCounters {
  std::uint64_t events_applied = 0;
  std::uint64_t top_level_changes = 0;    // events that changed the top levels
  std::uint64_t refused_adds = 0;         // each marks its instrument kInstrumentBad
  std::uint64_t unknown_instruments = 0;  // adds for an instrument the book does not have
  std::uint64_t unknown_orders = 0;       // other events for an order the book does not have
};

// One per process, single-threaded: only the feed thread calls it.
// - Every structure is sized and touched in the constructor, so apply never allocates.
template <class BookConfig = FinalBookConfig>
class FeedBook {
 private:
  static constexpr std::size_t kPrefetchEventsAhead = BookConfig::kPrefetchEventsAhead;

  // Everything an event reads per instrument besides the price ladder.
  // - Two 52 B TopLevels + 4 + 4 + 8 = 120 B, padded to two 64 B lines by alignas.
  struct alignas(kL1LineBytes) InstrumentState {
    TopLevels top_levels[2];  // index 0 bid, 1 ask: the values of Side
    std::int32_t reference_price = 0;
    std::uint32_t instrument_flags = 0;
    std::uint64_t exchange_time_ns = 0;
  };
  static_assert(sizeof(InstrumentState) == 128);

  // The shown levels of the side an event touched, as they were before its first change.
  // - One copy is enough: all of an event's changes fall on one side of one instrument (a
  //   replace keeps the side).
  struct TopLevelsBeforeEvent {
    TopLevels before;
    std::uint8_t side = 0;
    bool has_before_copy = false;
  };

  std::vector<InstrumentState> instrument_states;  // one per instrument
  // every price level's total, per side per instrument
  typename BookConfig::LevelPolicy price_levels;
  // every live order: id -> qty, instrument, side, price
  typename BookConfig::OrderIndexPolicy live_order_index;
  typename BookConfig::LookupPolicy instrument_lookup;  // finds an event's instrument
  BookCounters book_counters{};

 public:
  // Startup only. The order table gets 2^order_table_log2 entries.
  FeedBook(const std::vector<InstrumentInfo>& instrument_list, unsigned order_table_log2)
      : instrument_states(checked_count(instrument_list)),
        price_levels(static_cast<std::uint32_t>(instrument_list.size())),
        live_order_index(order_table_log2),
        instrument_lookup(instrument_list) {
    for (std::size_t i = 0; i < instrument_list.size(); ++i)
      instrument_states[i].reference_price = instrument_list[i].reference_price;
  }

  FeedBook(const FeedBook&) = delete;
  FeedBook& operator=(const FeedBook&) = delete;

  // Applies one exchange event and returns what consumers must hear about it.
  // - An event for an unknown instrument or order changes nothing and is only counted.
  // Example (instrument 0, reference_price 20000, an empty book):
  //   Add 7, bid 20005 x100      -> delta bid {20005, 100}
  //   Execute 7, qty 40          -> trade 20005 x40, seller aggressor; delta bid {20005, 60}
  //   Cancel 7, qty 0 (all)      -> delta bid {20005, 0}
  //   Add 8, bid 30000 x10       -> no delta; reject PriceOutsideWindow, became_bad
  BookOutput apply(const OrderEvent& event) {
    ++book_counters.events_applied;
    BookOutput output;
    TopLevelsBeforeEvent change;

    // 1. Find the instrument (an add names it; other events take it from the order), then
    //    add, reduce or replace.
    switch (event.type) {
      case EventType::Add:
        output.instrument_id = instrument_lookup.find_instrument(event);
        if (output.instrument_id == kNoInstrument) {
          ++book_counters.unknown_instruments;
        } else {
          const auto side = static_cast<std::uint8_t>(event.side);
          add_order(output.instrument_id, side, event.order_id, event.price, event.qty, output,
                    change);
        }
        break;
      case EventType::Cancel:
      case EventType::Execute:
      case EventType::Replace:
        reduce_existing_order(event, output, change);
        break;
    }

    // 2. Build the delta from the before-copy and the levels now.
    if (output.instrument_id != kNoInstrument)
      build_delta(output, change, event.exchange_time_ns);
    return output;
  }

  // Applies events in order and calls handle_output(i, output) right after event i.
  // - The handler sees the book exactly as of event i, so it can fill a snapshot there.
  // - Prefetches the order-table entry kPrefetchEventsAhead events ahead, within the batch: its
  //   first kPrefetchEventsAhead events are never prefetched.
  template <class OutputHandler>
  void apply_batch(const OrderEvent* events, std::size_t event_count,
                   OutputHandler&& handle_output) {
    for (std::size_t i = 0; i < event_count; ++i) {
      if (kPrefetchEventsAhead > 0 && i + kPrefetchEventsAhead < event_count) {
        live_order_index.prefetch(events[i + kPrefetchEventsAhead].order_id);
      }
      handle_output(i, apply(events[i]));
    }
  }

  // instrument_id's top levels, flags and time. The publisher sets last_included_seq.
  void fill_snapshot(std::uint16_t instrument_id, InstrumentSnapshot& snapshot) const {
    const InstrumentState& state = instrument_states[instrument_id];
    snapshot = InstrumentSnapshot{};
    snapshot.exchange_time_ns = state.exchange_time_ns;
    snapshot.instrument_id = instrument_id;
    snapshot.bid_count = state.top_levels[0].count;
    snapshot.ask_count = state.top_levels[1].count;
    snapshot.instrument_flags = state.instrument_flags;
    for (std::size_t k = 0; k < kTopLevelsPerSide; ++k) {
      snapshot.bids[k] = state.top_levels[0].levels[k];
      snapshot.asks[k] = state.top_levels[1].levels[k];
    }
  }

  std::uint32_t instrument_count() const {
    return static_cast<std::uint32_t>(instrument_states.size());
  }

  const BookCounters& counters() const {
    return book_counters;
  }

  // - Test-only: one side's shown levels, and the order index (for its invariant check).
  const TopLevels& top_levels(std::uint16_t instrument_id, std::uint8_t side) const {
    return instrument_states[instrument_id].top_levels[side];
  }

  const typename BookConfig::OrderIndexPolicy& order_index() const {
    return live_order_index;
  }

 private:
  // Checked before anything is allocated.
  static std::size_t checked_count(const std::vector<InstrumentInfo>& instrument_list) {
    check_or_abort(!instrument_list.empty() && instrument_list.size() < kNoInstrument,
                   "FeedBook: 1 to 65534 instruments");
    return instrument_list.size();
  }

  // Checks a new resting order, stores it, and adds its qty to its price level.
  // - The checks run in RejectReason order; the first that fails refuses the add, marks the
  //   instrument kInstrumentBad and changes nothing else.
  void add_order(std::uint16_t instrument_id, std::uint8_t side, std::uint64_t id,
                 std::int32_t price, std::uint32_t qty, BookOutput& output,
                 TopLevelsBeforeEvent& change) {
    InstrumentState& state = instrument_states[instrument_id];
    // Computed in 64 bits so that a far-off price cannot overflow before the range check.
    const std::int64_t wide_offset = std::int64_t{price} - state.reference_price;

    if (side > 1 || qty == 0 || id == 0) {
      refuse_add(state, output, RejectReason::Malformed);
      return;
    }
    if (wide_offset < kMinPriceOffset || wide_offset > kMaxPriceOffset) {
      refuse_add(state, output, RejectReason::PriceOutsideWindow);
      return;
    }
    if (live_order_index.live_orders() >= live_order_index.max_live_orders()) {
      refuse_add(state, output, RejectReason::OrderTableFull);
      return;
    }

    const auto price_offset = static_cast<std::int32_t>(wide_offset);
    const OrderRecord record{qty, instrument_id, side, price_offset};
    if (!live_order_index.insert(id, record)) {
      refuse_add(state, output, RejectReason::Duplicate);
      return;
    }

    // The price is inside the window (checked above), so the level always has a slot.
    const std::uint32_t total = price_levels.add_qty(instrument_id, side, price_offset, qty);
    if (total < qty) {  // the level's total wrapped past 2^32 - 1
      take_back_wrapped_add(state, id, record, output);
      return;
    }
    update_top_levels(instrument_id, side, price, total, change);
  }

  // Takes the add back out of the ladder and the order index, then refuses it: the level held
  // more than 0 before, so it keeps that total and stays non-empty. Cold and out of line, so the
  // add path does not carry it.
  [[gnu::cold, gnu::noinline]] void take_back_wrapped_add(InstrumentState& state, std::uint64_t id,
                                                          OrderRecord record, BookOutput& output) {
    price_levels.remove_qty(record.instrument_id, record.side, record.price_offset,
                            record.remaining_qty);
    live_order_index.erase(live_order_index.find(id));
    refuse_add(state, output, RejectReason::LevelTotalOverflow);
  }

  // Cancel, execute or replace: takes qty off the order and off its price level.
  // - An execute also reports the trade; a replace then adds the new order.
  // - An unknown order id is counted and ignored (its add may have been in a lost packet).
  void reduce_existing_order(const OrderEvent& event, BookOutput& output,
                             TopLevelsBeforeEvent& change) {
    const auto entry = live_order_index.find(event.order_id);
    if (!live_order_index.is_found(entry)) {
      ++book_counters.unknown_orders;
      return;
    }
    const OrderRecord record = live_order_index.record(entry);
    const bool is_cancel = event.type == EventType::Cancel;
    const bool is_execute = event.type == EventType::Execute;
    const bool is_replace = event.type == EventType::Replace;
    output.instrument_id = record.instrument_id;

    // Cancel 0 means all; executing 0 is a no-op. Nothing removes more than the order holds:
    // the exchange never sends that, and cutting it to fit keeps the quantity from wrapping.
    std::uint32_t remove_qty = event.qty;
    const bool remove_all = is_replace || (is_cancel && event.qty == 0);
    if (remove_all || remove_qty > record.remaining_qty) remove_qty = record.remaining_qty;
    if (remove_qty == 0) return;

    if (remove_qty == record.remaining_qty) {
      live_order_index.erase(entry);
    } else {
      live_order_index.record(entry).remaining_qty = record.remaining_qty - remove_qty;
    }

    const std::uint8_t side = record.side;
    const std::int32_t price_offset = record.price_offset;
    const std::int32_t price =
        instrument_states[output.instrument_id].reference_price + price_offset;

    if (is_execute) {
      output.trade.price = price;
      output.trade.qty = event.qty;  // what the exchange reported, even if more than the book held
      // The aggressor is the side opposite the resting order: a resting bid was hit by a seller.
      output.trade.aggressor = side == 0 ? kSellerAggressor : kBuyerAggressor;
    }

    const std::uint32_t left =
        price_levels.remove_qty(output.instrument_id, side, price_offset, remove_qty);
    update_top_levels(output.instrument_id, side, price, left, change);
    // A replace is two legs, as on the exchange: the old order is gone before the new one is
    // checked. If the book refuses the new leg, the old one stays gone, which matches what the
    // exchange did, and the refusal marks the instrument kInstrumentBad like any refused add.
    if (is_replace) {
      add_order(output.instrument_id, side, event.new_order_id, event.price, event.qty, output,
                change);
    }
  }

  // Counts the refusal and marks the instrument kInstrumentBad; became_bad is set only the
  // first time, since the flag is never cleared.
  void refuse_add(InstrumentState& state, BookOutput& output, RejectReason reason) {
    ++book_counters.refused_adds;
    output.reject_reason = reason;
    if ((state.instrument_flags & kInstrumentBad) == 0) {
      state.instrument_flags |= kInstrumentBad;
      output.became_bad = true;
    }
  }

  // The level at price now holds total (0: gone); brings that side's shown levels up to date.
  // - Saves a copy of them the first time an event changes them, for build_delta.
  // - A full side that loses a level refills its 6th from the ladder (next_worse_level).
  void update_top_levels(std::uint16_t instrument_id, std::uint8_t side, std::int32_t price,
                         std::uint32_t total, TopLevelsBeforeEvent& change) {
    InstrumentState& state = instrument_states[instrument_id];
    TopLevels& side_levels = state.top_levels[side];
    const bool full = side_levels.count == kTopLevelsPerSide;
    const bool is_bid = side == 0;
    const std::int32_t worst_price = side_levels.levels[kTopLevelsPerSide - 1].price;

    change.side = side;
    // 1. A level worse than the 6th of a full side is not shown: nothing to do, no copy.
    if (full && is_better_price(is_bid, worst_price, price)) return;
    // 2. Keep the before-copy, then apply the change.
    if (!change.has_before_copy) {
      change.before = side_levels;
      change.has_before_copy = true;
    }
    apply_level_change(side_levels, is_bid, Level{price, total});

    // 3. A full side lost a level: refill the 6th with the best level worse than the old 6th.
    if (full && side_levels.count < kTopLevelsPerSide) {
      std::uint32_t qty = 0;
      const std::int32_t worst_offset = worst_price - state.reference_price;
      const std::int32_t refill_offset =
          price_levels.next_worse_level(instrument_id, side, worst_offset, qty);
      if (refill_offset != kNoLevel) {
        side_levels.levels[side_levels.count] = Level{state.reference_price + refill_offset, qty};
        side_levels.count = static_cast<std::uint8_t>(side_levels.count + 1);
      }
    }
  }

  // Writes output.delta as the diff of the before-copy and the levels now, and stamps the
  // instrument's time if anything consumers see changed.
  void build_delta(BookOutput& output, const TopLevelsBeforeEvent& change,
                   std::uint64_t exchange_time_ns) {
    InstrumentState& state = instrument_states[output.instrument_id];
    Level changes[2 * kTopLevelsPerSide];
    std::size_t count = 0;

    if (change.has_before_copy)
      count = diff_top_levels(change.before, state.top_levels[change.side], changes);

    // At most 3 entries; diff_top_levels names the two cases, both from a replace.
    check_or_abort(count <= BookDelta::kMaxEntries, "a replace changes at most 3 levels");

    output.delta.side = count != 0 ? change.side : 0;
    output.delta.entry_count = static_cast<std::uint8_t>(count);
    for (std::size_t k = 0; k < count; ++k) output.delta.entries[k] = changes[k];
    if (count != 0) ++book_counters.top_level_changes;
    // The snapshot's time is that of the last change consumers see (a shown level, or the bad
    // flag): a trade outside the shown levels leaves it alone.
    if (count != 0 || output.became_bad) state.exchange_time_ns = exchange_time_ns;
  }
};

}  // namespace mdbus::book
