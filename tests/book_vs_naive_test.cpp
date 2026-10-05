// The book against two references.
// - A simple model that shares no code with FeedBook: a std::map of levels per side. After every
//   event the final book's top levels must equal the model's first K.
// - The differential oracle: the old F0 book and each ladder config replay one event stream in
//   lock step. After every event the config's output and the changed instrument's snapshot must
//   equal F0's byte for byte; at the end every snapshot must, and the order index must pass its
//   reachability check.
// - Hand-built events each config must refuse exactly as F0 does: prices outside the window, a
//   duplicate id, an order past the live cap, a bad side.
// - F0 shares FeedBook's top-K code with every config, which is why the model comes first.

#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <unordered_map>
#include <vector>

#include "baseline/book_ladder.hpp"
#include "sim/order_event_generator.hpp"
#include "test_harness.hpp"

using namespace mdbus;
using namespace mdbus::baseline;
using namespace mdbus::book;

namespace {

constexpr std::size_t kEventsPerBatch = 64;

// The model's side: price -> total quantity.
using ModelLevels = std::map<std::int32_t, std::int64_t>;

struct ModelOrder {
  std::uint16_t instrument_id;
  std::uint8_t side;
  std::int32_t price;
  std::uint32_t qty;
};

// The top levels equal the model's first K levels, best first.
template <class LevelIterator>
bool top_levels_match_model(const TopLevels& top, LevelIterator level, LevelIterator end) {
  std::size_t k = 0;
  for (; level != end && k < kTopLevelsPerSide; ++level, ++k) {
    if (top.levels[k].price != level->first || top.levels[k].qty != level->second) return false;
  }
  return top.count == k;
}

// The simple model: every live order, and per instrument and side the total qty at each price.
struct MapModel {
  std::unordered_map<std::uint64_t, ModelOrder> orders;
  std::vector<std::array<ModelLevels, 2>> levels;  // [instrument_id][side]

  explicit MapModel(std::uint32_t instrument_count) : levels(instrument_count) {}

  // Applies one generated event (always a valid one); returns the instrument it changed.
  std::uint16_t apply(const OrderEvent& e) {
    if (e.type == EventType::Add) {
      const ModelOrder o{e.instrument_id, static_cast<std::uint8_t>(e.side), e.price, e.qty};
      orders[e.order_id] = o;
      add_to_level(o, o.qty);
      return e.instrument_id;
    }

    ModelOrder o = orders.at(e.order_id);
    orders.erase(e.order_id);
    std::uint32_t q = e.qty;
    const bool remove_all = e.type == EventType::Replace || (e.type == EventType::Cancel && q == 0);
    if (remove_all || q > o.qty) q = o.qty;
    add_to_level(o, -std::int64_t{q});
    o.qty -= q;
    if (e.type == EventType::Replace) {
      o.price = e.price;
      o.qty = e.qty;
      add_to_level(o, o.qty);
      orders[e.new_order_id] = o;
    } else if (o.qty > 0) {
      orders[e.order_id] = o;
    }
    return o.instrument_id;
  }

 private:
  void add_to_level(const ModelOrder& o, std::int64_t qty) {
    ModelLevels& side = levels[o.instrument_id][o.side];
    side[o.price] += qty;
    if (side[o.price] == 0) side.erase(o.price);
  }
};

// Replays `events` generated events through the final book and the model; returns how many
// events left the changed instrument's top K different from the model's.
std::uint64_t count_mismatches_vs_map_model(const GeneratorConfig& config, std::uint64_t events) {
  OrderEventGenerator generator(config);
  FeedBook<> book(generator.instrument_list(), 16);
  MapModel model(config.instrument_count);

  std::uint64_t mismatches = 0;
  for (std::uint64_t i = 0; i < events; ++i) {
    const OrderEvent e = generator.next_event();
    book.apply(e);
    const std::uint16_t instrument_id = model.apply(e);

    const ModelLevels& bids = model.levels[instrument_id][0];
    const ModelLevels& asks = model.levels[instrument_id][1];
    if (!top_levels_match_model(book.top_levels(instrument_id, 0), bids.rbegin(), bids.rend()) ||
        !top_levels_match_model(book.top_levels(instrument_id, 1), asks.begin(), asks.end())) {
      ++mismatches;
    }
  }
  return mismatches;
}

// Replays `events` generated events through F0 and BookConfig; returns how many events (and final
// snapshots) differed.
template <class BookConfig>
std::uint64_t count_mismatches_vs_f0(const GeneratorConfig& config, unsigned table_log2,
                                     std::uint64_t events) {
  OrderEventGenerator generator(config);
  FeedBook<F0> naive(generator.instrument_list(), table_log2);
  FeedBook<BookConfig> book(generator.instrument_list(), table_log2);
  std::uint64_t bad = 0;
  OrderEvent ev[kEventsPerBatch];
  BookOutput want[kEventsPerBatch];
  InstrumentSnapshot f0_snapshots[kEventsPerBatch];

  for (std::uint64_t done = 0; done < events; done += kEventsPerBatch) {
    generator.fill_events(ev, kEventsPerBatch);
    naive.apply_batch(ev, kEventsPerBatch, [&](std::size_t i, const BookOutput& out) {
      want[i] = out;
      f0_snapshots[i] = InstrumentSnapshot{};
      if (out.instrument_id != kNoInstrument)
        naive.fill_snapshot(out.instrument_id, f0_snapshots[i]);
    });

    book.apply_batch(ev, kEventsPerBatch, [&](std::size_t i, const BookOutput& out) {
      InstrumentSnapshot snapshot{};
      if (out.instrument_id != kNoInstrument) book.fill_snapshot(out.instrument_id, snapshot);
      const bool same_output = std::memcmp(&out, &want[i], sizeof out) == 0;
      const bool same_snapshot = std::memcmp(&snapshot, &f0_snapshots[i], sizeof snapshot) == 0;
      if (same_output && same_snapshot) return;
      if (bad == 0) {
        std::cerr << "  " << BookConfig::kName << ": mismatch at order " << ev[i].order_id
                  << ": reject " << int(out.reject_reason) << " vs " << int(want[i].reject_reason)
                  << '\n';
      }
      ++bad;
    });
  }

  for (std::uint16_t i = 0; i < naive.instrument_count(); ++i) {
    InstrumentSnapshot x;
    InstrumentSnapshot y;
    naive.fill_snapshot(i, x);
    book.fill_snapshot(i, y);
    if (std::memcmp(&x, &y, sizeof x) != 0) ++bad;
  }
  if (!book.order_index().check_invariants()) ++bad;

  const BookCounters& s = naive.counters();
  std::cerr << "  " << BookConfig::kName << ": " << s.events_applied << " events, "
            << s.top_level_changes << " top changes, " << s.refused_adds << " rejects\n";
  CHECK(s.top_level_changes > events / 10);
  CHECK(s.refused_adds == 0);
  return bad;
}

// Feeds one hand-built event to F0 and to BookConfig, and checks that both give the same output and
// snapshot, and the expected reject code.
template <class BookConfig>
bool results_match_f0(FeedBook<F0>& naive, FeedBook<BookConfig>& book, const OrderEvent& ev,
                 RejectReason expect) {
  const BookOutput want = naive.apply(ev);
  const BookOutput got = book.apply(ev);
  if (std::memcmp(&want, &got, sizeof want) != 0) return false;
  if (got.reject_reason != expect) return false;
  if (got.instrument_id == kNoInstrument) return true;
  InstrumentSnapshot x;
  InstrumentSnapshot y;
  naive.fill_snapshot(got.instrument_id, x);
  book.fill_snapshot(got.instrument_id, y);
  return std::memcmp(&x, &y, sizeof x) == 0;
}

OrderEvent add_event(const InstrumentInfo& def, std::uint16_t instrument_id, std::uint64_t id,
                     std::int32_t price, Side side) {
  OrderEvent e{};
  e.type = EventType::Add;
  e.order_id = id;
  e.price = price;
  e.qty = 100;
  e.instrument_id = instrument_id;
  e.side = side;
  e.symbol = def.symbol;
  return e;
}

// Hand-built adds that BookConfig must accept or refuse exactly as F0 does, with the same reason.
template <class BookConfig>
void check_refusals() {
  std::vector<InstrumentInfo> instrument_list(2);
  instrument_list[0].symbol = symbol_for_id(0);
  instrument_list[0].reference_price = 10'000;
  instrument_list[1].symbol = symbol_for_id(1);
  instrument_list[1].reference_price = 20'000;

  constexpr unsigned kSmallestTableLog2 = 4;  // the smallest table: 16 entries, a live cap of 12
  FeedBook<F0> naive(instrument_list, kSmallestTableLog2);
  FeedBook<BookConfig> book(instrument_list, kSmallestTableLog2);
  const std::int32_t ref = instrument_list[0].reference_price;

  // The window's edges are accepted on both sides; one tick past either is refused.
  OrderEvent e = add_event(instrument_list[0], 0, 1, ref + kMaxPriceOffset, Side::Ask);
  CHECK(results_match_f0(naive, book, e, RejectReason::None));
  e = add_event(instrument_list[0], 0, 2, ref + kMinPriceOffset, Side::Bid);
  CHECK(results_match_f0(naive, book, e, RejectReason::None));
  e = add_event(instrument_list[0], 0, 3, ref + kMaxPriceOffset + 1, Side::Ask);
  CHECK(results_match_f0(naive, book, e, RejectReason::PriceOutsideWindow));
  e = add_event(instrument_list[0], 0, 4, ref + kMinPriceOffset - 1, Side::Bid);
  CHECK(results_match_f0(naive, book, e, RejectReason::PriceOutsideWindow));

  // Order 1 is live: its id again, on the other instrument.
  e = add_event(instrument_list[1], 1, 1, instrument_list[1].reference_price + 5, Side::Ask);
  CHECK(results_match_f0(naive, book, e, RejectReason::Duplicate));
  e = add_event(instrument_list[1], 1, 5, instrument_list[1].reference_price, Side::Bid);
  e.side = static_cast<Side>(2);
  CHECK(results_match_f0(naive, book, e, RejectReason::Malformed));

  // Orders 1 and 2 are live. Fill to the live cap with bids one tick apart, then one more.
  const std::uint32_t cap = max_live_orders_for(kSmallestTableLog2);
  for (std::uint32_t live = 2; live < cap; ++live) {
    const std::int32_t price = ref - static_cast<std::int32_t>(live);
    e = add_event(instrument_list[0], 0, 100 + live, price, Side::Bid);
    CHECK(results_match_f0(naive, book, e, RejectReason::None));
  }
  e = add_event(instrument_list[0], 0, 100 + cap, ref - 100, Side::Bid);
  CHECK(results_match_f0(naive, book, e, RejectReason::OrderTableFull));

  CHECK(naive.counters().refused_adds == 5 && book.counters().refused_adds == 5);
}

}  // namespace

// The final book against the std::map model, after every event.
TEST(final_book_matches_a_simple_model) {
  GeneratorConfig config;
  config.seed = 5;
  CHECK(count_mismatches_vs_map_model(config, 300'000) == 0);
}

// Every config against F0 in lock step on one generated stream, byte for byte.
TEST(oracle_uniform) {
  GeneratorConfig config;
  config.seed = 11;
  CHECK(count_mismatches_vs_f0<F1>(config, 15, 256'000) == 0);
  CHECK(count_mismatches_vs_f0<F2>(config, 15, 256'000) == 0);
  CHECK(count_mismatches_vs_f0<F3>(config, 15, 256'000) == 0);
  CHECK(count_mismatches_vs_f0<F4>(config, 15, 256'000) == 0);
}

// The hand-built bad events: each config refuses them exactly as F0 does.
TEST(oracle_refusals) {
  check_refusals<F1>();
  check_refusals<F2>();
  check_refusals<F3>();
  check_refusals<F4>();
}

// An add for an instrument the book does not have and a cancel of an order it does not have are
// counted apart. A cancel larger than the order removes the whole order; so does an execution,
// whose trade still reports the size the exchange printed.
TEST(unknown_instruments_and_orders) {
  std::vector<InstrumentInfo> instrument_list(1);
  instrument_list[0].symbol = symbol_for_id(0);
  instrument_list[0].reference_price = 10'000;
  FeedBook<> book(instrument_list, 4);

  OrderEvent e = add_event(instrument_list[0], 5, 1, 10'000, Side::Bid);  // instrument 5 of 1
  CHECK(!book.apply(e).has_anything_to_publish());

  OrderEvent cancel{};
  cancel.type = EventType::Cancel;
  cancel.order_id = 2;  // never added
  CHECK(!book.apply(cancel).has_anything_to_publish());

  e = add_event(instrument_list[0], 0, 3, 10'000, Side::Bid);
  book.apply(e);
  cancel.order_id = 3;
  cancel.qty = 500;  // the order holds 100
  CHECK(book.apply(cancel).delta.entry_count == 1);  // cut to fit: the level is removed
  CHECK(book.top_levels(0, 0).count == 0);

  e = add_event(instrument_list[0], 0, 4, 10'000, Side::Bid);
  book.apply(e);
  OrderEvent execute{};
  execute.type = EventType::Execute;
  execute.order_id = 4;
  execute.qty = 300;  // the order holds 100
  const BookOutput traded = book.apply(execute);
  CHECK(traded.trade.qty == 300 && traded.delta.entry_count == 1 &&
        book.top_levels(0, 0).count == 0);

  const BookCounters& s = book.counters();
  CHECK(s.unknown_instruments == 1 && s.unknown_orders == 1);
  CHECK(s.events_applied == 6 && s.refused_adds == 0);
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
