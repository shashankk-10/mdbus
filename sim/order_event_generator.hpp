#pragma once

// The seeded order-event stream: the one workload source for the exchange simulator, the feed
// handler's instrument list, the tests and the feed bench.
// - Same seed, same stream: the simulator and the feed handler never exchange the instrument
//   list, they both derive it from --seed and --instruments.
// - Holds the live-order count at a target, so the book's order table works at a fixed size. The
//   stream opens with target_live_orders adds, so that size is reached before the first cancel.
// - Cancels and executions pick a random live order, which spreads them over the whole table.
// - In namespace mdbus::book, like the OrderEvent it produces.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "mdbus/book/order_event.hpp"
#include "mdbus/clock.hpp"
#include "mdbus/status.hpp"

namespace mdbus::book {

// Four capital letters, the id written in base 26, padded with spaces to 8 chars: 0 -> "AAAA",
// 1 -> "AAAB", 27 -> "AABB". Unique for every id below 26^4 = 456,976, so for every 16-bit id.
constexpr std::array<char, 8> symbol_for_id(std::uint32_t id) {
  constexpr std::size_t kSymbolLetters = 4;
  constexpr std::uint32_t kAlphabetSize = 26;
  std::array<char, 8> symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  for (std::size_t i = kSymbolLetters; i > 0; --i) {
    symbol[i - 1] = static_cast<char>('A' + id % kAlphabetSize);
    id /= kAlphabetSize;
  }
  return symbol;
}

// Set from the command line by the simulator and the feed handler (--seed, --instruments).
struct GeneratorConfig {
  std::uint64_t seed = 1;
  std::uint32_t instrument_count = 64;
  std::uint32_t target_live_orders = 16 * 1024;  // live orders across all instruments
};

// Reference prices lie in [kMinReferencePrice, kMinReferencePrice + kReferencePriceRange), in
// ticks.
constexpr std::int32_t kMinReferencePrice = 20'000;
constexpr std::int32_t kReferencePriceRange = 40'000;

// The instrument list the simulator and the feed handler both derive from the seed: instrument i
// gets symbol_for_id(i) and a reference price from one draw of random_engine.
// - The generator calls this as the first use of its engine, so a feed handler that seeds an
//   mt19937_64 with the same seed gets the same list.
inline std::vector<InstrumentInfo> make_instrument_list(std::mt19937_64& random_engine,
                                                        std::uint32_t instrument_count) {
  std::vector<InstrumentInfo> instrument_list;
  for (std::uint32_t i = 0; i < instrument_count; ++i) {
    InstrumentInfo info{};
    info.symbol = symbol_for_id(i);
    info.reference_price =
        kMinReferencePrice + static_cast<std::int32_t>(random_engine() % kReferencePriceRange);
    instrument_list.push_back(info);
  }
  return instrument_list;
}

// Produces the order events, one per next_event() call. Allocates nothing after construction.
class OrderEventGenerator {
 private:
  // The decision tree in next_event() uses these (each compared with one uniform() draw).
  static constexpr double kAddProbability = 0.5;                // at the target: add
  static constexpr double kPartialReduceProbability = 0.5;      // else: reduce part of an order
  static constexpr double kReplaceWhenEndingProbability = 0.1;  // ending: replace
  static constexpr double kExecuteWhenEndingProbability = 0.2;  // ending: execute all, else cancel

  // Mean distance, in ticks, between an order's price and its instrument's reference price.
  // - The reference never moves: the simulator never matches crossing orders, so a moving price
  //   would leave old bids above new asks (a crossed book).
  static constexpr double kMeanTicksFromReference = 8.0;

  // A fake exchange clock, unrelated to the send rate: 09:30, then 250 ns per event.
  static constexpr std::uint64_t kMarketOpenNsAfterMidnight = (9 * 3600 + 30 * 60) * kNsPerSecond;
  static constexpr std::uint64_t kExchangeTimeStepNs = 250;

  // An order's size is 1 to kMaxLots lots of kLotSize shares.
  static constexpr std::uint32_t kLotSize = 100;
  static constexpr std::uint32_t kMaxLots = 10;

  static constexpr int kDoubleMantissaBits = 53;  // a double holds every integer below 2^53

  // What the generator remembers about an order it has added and not yet ended.
  struct LiveOrder {
    std::uint64_t id;
    std::int32_t price;
    std::uint32_t qty;  // what is left after partial reductions
    std::uint16_t instrument_id;
    Side side;
  };

  GeneratorConfig config;
  std::mt19937_64 random_engine;
  std::vector<InstrumentInfo> instruments_by_id;
  std::vector<LiveOrder> live_orders;  // in no particular order
  std::uint64_t events_generated = 0;
  std::uint64_t next_order_id = 1;

 public:
  explicit OrderEventGenerator(const GeneratorConfig& settings)
      : config(settings), random_engine(settings.seed) {
    check_or_abort(config.instrument_count > 0 && config.instrument_count < kNoInstrument,
                   "OrderEventGenerator: 1 to 65534 instruments");
    check_or_abort(config.target_live_orders > 0,
                   "OrderEventGenerator: target_live_orders > 0");

    instruments_by_id = make_instrument_list(random_engine, config.instrument_count);
    // The count never passes target + 1, so next_event() never grows the vector.
    live_orders.reserve(std::size_t{config.target_live_orders} + 1);
  }

  const std::vector<InstrumentInfo>& instrument_list() const {
    return instruments_by_id;
  }

  // The next event, stamped with the fake exchange time.
  // - Below the target count: add an order. Above it (target + 1): end a random live order.
  // - At the target: add with kAddProbability; else reduce part of a random live order with
  //   kPartialReduceProbability (a cancel or an execution, a coin flip); else end one.
  // - Ending an order: replace it with kReplaceWhenEndingProbability, execute all of it with
  //   kExecuteWhenEndingProbability, else cancel all of it.
  // - Measured mix (seed 1, the 200,000 events after the 16,384 opening adds): about 41% add,
  //   39% cancel, 16% execute, 4.5% replace.
  // Example (seed 1, default config):
  //   event 1:      Add, order_id 1, instrument 15 ("AAAP"), Ask, price 43854 (reference
  //                 43833), qty 1000, exchange_time_ns 34'200'000'000'250
  //   event 2:      Add, order_id 2, instrument 34 ("AABI"), Bid, price 44034 (reference
  //                 44046), qty 300, exchange_time_ns 34'200'000'000'500
  //   event 16,385: the first at the target: Cancel, order_id 8115, instrument 19, qty 100
  OrderEvent next_event() {
    OrderEvent event{};
    const std::size_t live_count = live_orders.size();

    if (live_count < config.target_live_orders) {
      add_order(event);
    } else if (live_count > config.target_live_orders) {
      end_order(event, random_live_index());
    } else if (uniform() < kAddProbability) {
      add_order(event);
    } else if (uniform() < kPartialReduceProbability) {
      reduce_order(event, random_live_index());
    } else {
      end_order(event, random_live_index());
    }

    ++events_generated;
    event.exchange_time_ns = kMarketOpenNsAfterMidnight + events_generated * kExchangeTimeStepNs;
    return event;
  }

  // The next event_count events into events[0..event_count).
  void fill_events(OrderEvent* events, std::size_t event_count) {
    for (std::size_t i = 0; i < event_count; ++i) {
      events[i] = next_event();
    }
  }

 private:
  // A new order on a random instrument and side, remembered in live_orders.
  void add_order(OrderEvent& event) {
    const auto instrument_id =
        static_cast<std::uint16_t>(random_engine() % config.instrument_count);
    Side side = Side::Bid;
    if (coin_flip()) side = Side::Ask;

    LiveOrder order{};
    order.id = next_order_id;
    ++next_order_id;
    order.price = new_price(instrument_id, side);
    order.qty = new_qty();
    order.instrument_id = instrument_id;
    order.side = side;
    live_orders.push_back(order);

    event.type = EventType::Add;
    event.order_id = order.id;
    event.price = order.price;
    event.qty = order.qty;
    event.instrument_id = instrument_id;
    event.side = side;
    event.symbol = instruments_by_id[instrument_id].symbol;
  }

  // The order's last event: a replace, an execution of all of it, or a cancel of all of it.
  // - A replace gives the order a new id, price and size on the same side, so the live count
  //   stays the same; the other two remove it.
  // - Half the full cancels say "all" (qty 0), half name the remaining size, so the book is
  //   tested on both forms.
  void end_order(OrderEvent& event, std::size_t live_index) {
    LiveOrder& order = live_orders[live_index];
    event.order_id = order.id;
    event.instrument_id = order.instrument_id;  // every event carries its instrument id
    const double ending_draw = uniform();
    if (ending_draw < kReplaceWhenEndingProbability) {
      event.type = EventType::Replace;
      order.id = next_order_id;
      ++next_order_id;
      order.price = new_price(order.instrument_id, order.side);
      order.qty = new_qty();
      event.new_order_id = order.id;
      event.price = order.price;
      event.qty = order.qty;
      return;
    }

    if (ending_draw < kReplaceWhenEndingProbability + kExecuteWhenEndingProbability) {
      event.type = EventType::Execute;
      event.qty = order.qty;
    } else {
      event.type = EventType::Cancel;
      event.qty = order.qty;
      if (coin_flip()) event.qty = 0;
    }

    // Remove by moving the last order into this place: O(1), and the order of live_orders does
    // not matter because victims are drawn at random.
    live_orders[live_index] = live_orders.back();
    live_orders.pop_back();
  }

  // Cancels or executes 1 to half of what is left; an order down to one share is ended instead.
  void reduce_order(OrderEvent& event, std::size_t live_index) {
    LiveOrder& order = live_orders[live_index];
    if (order.qty < 2) {
      end_order(event, live_index);
      return;
    }

    event.qty = 1 + static_cast<std::uint32_t>(random_engine() % (order.qty / 2));
    order.qty -= event.qty;
    event.type = EventType::Cancel;
    if (coin_flip()) event.type = EventType::Execute;
    event.order_id = order.id;
    event.instrument_id = order.instrument_id;
  }

  std::size_t random_live_index() {
    return static_cast<std::size_t>(random_engine() % live_orders.size());
  }

  // At least one tick from the reference on the order's own side (bids below, asks above), at
  // an exponential distance with mean about kMeanTicksFromReference, so the book never crosses.
  std::int32_t new_price(std::uint16_t instrument_id, Side side) {
    const double exponential_ticks = -kMeanTicksFromReference * std::log1p(-uniform());
    const std::int32_t ticks_from_reference = 1 + static_cast<std::int32_t>(exponential_ticks);
    if (side == Side::Bid) {
      return instruments_by_id[instrument_id].reference_price - ticks_from_reference;
    }
    return instruments_by_id[instrument_id].reference_price + ticks_from_reference;
  }

  // One draw: true for an odd number.
  bool coin_flip() {
    return random_engine() % 2 == 1;
  }

  std::uint32_t new_qty() {
    return kLotSize * (1 + static_cast<std::uint32_t>(random_engine() % kMaxLots));
  }

  // A double in [0, 1) from the top 53 bits of one draw.
  // - Why not std::uniform_real_distribution: <random>'s distribution algorithms differ between
  //   standard libraries, while mt19937_64's output is fixed by the standard. Computed here, the
  //   stream depends only on the seed and libm's log1p.
  double uniform() {
    constexpr int kEngineBits = 64;
    constexpr double kTwoToTheMantissaBits =
        static_cast<double>(std::uint64_t{1} << kDoubleMantissaBits);
    return static_cast<double>(random_engine() >> (kEngineBits - kDoubleMantissaBits)) /
           kTwoToTheMantissaBits;
  }
};

}  // namespace mdbus::book
