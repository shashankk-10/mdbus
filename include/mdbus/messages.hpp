#pragma once

// The three bus messages, and Schema, the typelist that checks and dispatches them.
// - The book produces them (book/publish_book_output.hpp), Publisher copies one into a slot's
//   body after the MessageHeader, and Consumer turns the header's type_id back into the type
//   with Schema::dispatch.
// - Each message is a plain struct with its padding written out as fields, so no byte of a
//   message is left undefined and the payload checksum over it is the same every time. The
//   size assert under each one shows the compiler added no padding of its own.
// - BusLayout (bus_layout.hpp) feeds each message's id and size into the layout hash.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mdbus {

// Building blocks.

// One price level.
// - price: integer price units, counted in the exchange's minimum price step (e.g. 10025 =
//   100.25 with a 0.01 step); not clock ticks. Why not a double: memcmp, the checksum and
//   exact book compares all need one bit pattern per price.
// - qty: the level's absolute total quantity, not a change; 0 removes the level.
struct Level {
  std::int32_t price;
  std::uint32_t qty;
};
static_assert(sizeof(Level) == 8);  // 4 + 4

// Bits in InstrumentStatus::instrument_flags and in every snapshot's instrument_flags.
// - kInstrumentBad: the book refused an event for this instrument.
// - kInstrumentSuspect: the feed saw a sequence gap, so the book may be missing events.
enum InstrumentFlag : std::uint8_t { kInstrumentBad = 1 << 0, kInstrumentSuspect = 1 << 1 };
// Either bit makes a snapshot useless for recovery.
constexpr std::uint32_t kSnapshotUnusableFlags = kInstrumentBad | kInstrumentSuspect;

// Trade::aggressor values: which side crossed the spread.
enum Aggressor : std::uint8_t { kBuyerAggressor = 0, kSellerAggressor = 1 };

// The messages.
// Type ids 1, 2, 3 go into the header's type_id byte.
// - 0 is reserved, so a never-written (all-zero) payload is never mistaken for a message.
// - Ids are part of the shared layout: changing one changes the layout hash.

// One book event's change to the top levels of one side.
// - side: a Side value (book/order_event.hpp), 0 Bid, 1 Ask. A plain byte, not the enum
//   class: the book indexes arrays by it.
// - entries[0 .. entry_count): the changed levels with their new absolute qty (0: gone).
struct BookDelta {
  static constexpr std::uint8_t kTypeId = 1;
  // One event changes at most 3 shown levels on one side: remove the old level of a replace,
  // update the new one, pull a 7th level up into view.
  static constexpr std::size_t kMaxEntries = 3;

  std::uint8_t side;
  std::uint8_t entry_count;
  std::uint8_t padding[2];
  Level entries[kMaxEntries];
};
static_assert(sizeof(BookDelta) == 28);  // 1 + 1 + 2 + 3 x 8

// An execution against a resting order.
// - aggressor: kBuyerAggressor (a buyer lifted an ask) or kSellerAggressor (a seller hit a
//   bid).
struct Trade {
  static constexpr std::uint8_t kTypeId = 2;

  std::int32_t price;
  std::uint32_t qty;
  std::uint8_t aggressor;
  std::uint8_t padding[3];
};
static_assert(sizeof(Trade) == 12);  // 4 + 4 + 1 + 3

// An instrument's InstrumentFlag bits changed; reject_reason is the book's reject code, if any.
struct InstrumentStatus {
  static constexpr std::uint8_t kTypeId = 3;

  std::uint8_t instrument_flags;
  std::uint8_t padding[3];
  std::uint32_t reject_reason;
};
static_assert(sizeof(InstrumentStatus) == 8);  // 1 + 3 + 4

// Types whose effect a snapshot already holds. During recovery Consumer drops only these when
// the snapshot's last_included_seq covers them.
// - Why not Trade: no snapshot stands in for a trade, so dropping one would lose it.
template <class T>
constexpr bool kIncludedInSnapshot = std::is_same_v<T, BookDelta>;

// A status message is what marks its instrument stale; if the stale filter could drop it, it
// would drop itself.
static_assert(!kIncludedInSnapshot<InstrumentStatus>,
              "InstrumentStatus must never be filtered by the snapshot");

// Copies a message out of a slot body. Through memcpy, never a pointer cast: the body is only
// 8-byte aligned and has no object of type T in it.
template <class T>
T read_message(const std::uint8_t* body) {
  T message;
  std::memcpy(&message, body, sizeof(T));
  return message;
}

// Schema.

// No two entries equal. A free function above Schema, because Schema's static_assert can only
// call what is already declared.
template <std::size_t Count>
constexpr bool type_ids_are_unique(const std::uint8_t (&type_ids)[Count]) {
  for (std::size_t i = 0; i < Count; ++i) {
    for (std::size_t j = i + 1; j < Count; ++j) {
      if (type_ids[i] == type_ids[j]) return false;
    }
  }
  return true;
}

// A typelist of messages, checked at compile time.
// - Each static_assert is a fold over the list, so adding a message that breaks a rule fails
//   to compile with the rule's reason.
// - The order of Messages matters: it is the dispatch order and the layout hash's input order.
template <class... Messages>
struct Schema {
  // Each message's id and size, in schema order (BusLayout::layout_hash reads both).
  static constexpr std::size_t kMessageCount = sizeof...(Messages);
  static constexpr std::uint8_t kTypeIds[] = {Messages::kTypeId...};
  static constexpr std::size_t kMessageBytes[] = {sizeof(Messages)...};

  static_assert(kMessageCount > 0);
  static_assert((std::is_trivially_copyable_v<Messages> && ...), "messages are copied with memcpy");
  static_assert(((alignof(Messages) <= 8) && ...), "a message body is only 8-byte aligned");
  static_assert(((Messages::kTypeId != 0) && ...), "id 0 is reserved");
  static_assert(type_ids_are_unique(kTypeIds), "message ids must be unique");

  // The largest message (BookDelta's 28 B in DefaultSchema); BusLayout asserts it fits the
  // 32 B slot body.
  static constexpr std::size_t kMaxMessageBytes = std::max({sizeof(Messages)...});
  // Publisher::encode_payload asserts this, so publishing a type outside the schema fails to
  // compile.
  template <class T>
  static constexpr bool kContains = (std::is_same_v<T, Messages> || ...);

  // Calls handler(message) for the type whose id matches; false if none does.
  // - A fold: one compare per type in schema order, stopping at the match. handler gets the
  //   concrete type, so it inlines and nothing goes through a pointer.
  // - Why not a virtual handler: it costs more instructions per message, since the message
  //   must go through memory to an unknown call (DESIGN.md, "A smaller result").
  // Example, with DefaultSchema (ids BookDelta 1, Trade 2, InstrumentStatus 3):
  //   dispatch(2, body, handler)  -> compares 1, then 2: calls handler(Trade), returns true
  //   dispatch(3, body, handler)  -> three compares: calls handler(InstrumentStatus), true
  //   dispatch(0, body, handler)  -> no match: handler is not called, returns false
  template <class Handler>
  static bool dispatch(std::uint8_t type_id, const std::uint8_t* body, Handler&& handler) {
    return (dispatch_if_type_matches<Messages>(type_id, body, handler) || ...);
  }

 private:
  template <class Message, class Handler>
  static bool dispatch_if_type_matches(std::uint8_t type_id, const std::uint8_t* body,
                                       Handler& handler) {
    if (type_id != Message::kTypeId) return false;
    handler(read_message<Message>(body));
    return true;
  }
};

// The bus's schema; BusLayout<> uses it.
using DefaultSchema = Schema<BookDelta, Trade, InstrumentStatus>;

}  // namespace mdbus
