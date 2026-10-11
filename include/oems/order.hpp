#pragma once
// An order and its lifecycle, as an explicit state machine. The only way to
// change an order's state or quantities is Order::apply(). A move that isn't in
// the transition table (order.cpp) is refused and leaves the order untouched.

#include "oems/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

namespace oems {

enum class OrdState : std::uint8_t {
  PendingNew,      // received, not yet accepted
  New,             // live, nothing filled
  PartiallyFilled, // live, some filled
  Filled,          // done: everything filled
  PendingCancel,   // client asked to cancel; waiting for the venue
  Canceled,        // done: cancelled, possibly after partial fills
  PendingReplace,  // client asked to change price/quantity; waiting for the venue
  Rejected,        // done: never accepted
};
inline constexpr std::size_t kOrdStateCount = 8;

// Events that move an order through its life. Requests come from the client;
// everything else comes from the venue (here, the in-process order book).
namespace event {
struct Accept {};          // venue acknowledged the new order
struct Reject {};          // new order refused (risk check or venue)
struct Fill {              // some or all of the order traded
  Qty qty;
  Price price;
};
struct CancelRequest {};   // client asked to cancel
struct CancelDone {};      // venue confirmed the cancel
struct CancelReject {};    // venue refused the cancel, e.g. the order had already filled
struct ReplaceRequest {};  // client asked to change price/quantity
struct ReplaceDone {       // venue applied the change
  Qty newOrderQty;
  Price newPrice;
};
struct ReplaceReject {};   // venue refused the change
struct VenueCancel {};     // venue cancelled unprompted, e.g. the unfilled rest of an IOC order
} // namespace event

using OrderEvent = std::variant<event::Accept, event::Reject, event::Fill, event::CancelRequest,
                                event::CancelDone, event::CancelReject, event::ReplaceRequest,
                                event::ReplaceDone, event::ReplaceReject, event::VenueCancel>;
inline constexpr std::size_t kEventCount = std::variant_size_v<OrderEvent>;

enum class ApplyResult : std::uint8_t {
  Ok,
  IllegalTransition, // the event isn't allowed in the order's current state
  InvalidQuantity,   // non-positive fill, or a replace below the filled quantity
  Overfill,          // a fill larger than the open quantity
};

class Order {
public:
  Order() = default;
  Order(OrderId id, SymbolId symbol, AccountId account, Side side, Price price, Qty orderQty)
      : id_(id), symbol_(symbol), account_(account), side_(side), price_(price), orderQty_(orderQty) {}

  // Applies one event. On anything but Ok, the order is left exactly as it was.
  [[nodiscard]] ApplyResult apply(const OrderEvent& event);

  OrderId id() const { return id_; }
  SymbolId symbol() const { return symbol_; }
  AccountId account() const { return account_; }
  Side side() const { return side_; }
  Price price() const { return price_; }
  Qty orderQty() const { return orderQty_; }
  Qty cumQty() const { return cumQty_; }   // filled so far; never decreases
  OrdState state() const { return state_; }
  Qty leavesQty() const;                   // still open; 0 once the order is done
  Price avgPx() const;                     // average fill price, rounded to 1/10,000; 0 before any fill

  bool operator==(const Order&) const = default;

private:
  OrderId id_ = 0;
  SymbolId symbol_ = 0;
  AccountId account_ = 0;
  Side side_ = Side::Buy;
  Price price_;
  Qty orderQty_ = 0;
  Qty cumQty_ = 0;
  Int128 fillNotional_ = 0; // sum of fill price.raw() × qty, for the average price
  OrdState state_ = OrdState::PendingNew;
};

bool isTerminal(OrdState state); // Filled, Canceled or Rejected

std::string_view toString(OrdState state);
std::string_view toString(ApplyResult result);
std::string_view eventName(std::size_t eventIndex);

} // namespace oems
