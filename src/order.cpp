#include "oems/order.hpp"

#include <array>
#include <cstdlib>
#include <type_traits>

namespace oems {
namespace {

// What an event does to an order in a given state.
enum class Next : std::uint8_t {
  X,                // illegal: refused, the order is unchanged
  Same,             // allowed, the state stays as it is
  ByQty,            // Filled if nothing is left open, else PartiallyFilled or New
  FilledIfDone,     // a fill while a request is pending: Filled if done, else unchanged
  ToNew,
  ToRejected,
  ToPendingCancel,
  ToCanceled,
  ToPendingReplace,
};
using enum Next;

// The table's columns follow the order of the OrderEvent variant.
template <typename E, std::size_t Column>
constexpr bool kColumnIs = std::is_same_v<std::variant_alternative_t<Column, OrderEvent>, E>;
static_assert(kColumnIs<event::Accept, 0> && kColumnIs<event::Reject, 1> &&
              kColumnIs<event::Fill, 2> && kColumnIs<event::CancelRequest, 3> &&
              kColumnIs<event::CancelDone, 4> && kColumnIs<event::CancelReject, 5> &&
              kColumnIs<event::ReplaceRequest, 6> && kColumnIs<event::ReplaceDone, 7> &&
              kColumnIs<event::ReplaceReject, 8> && kColumnIs<event::VenueCancel, 9>);

// The whole order lifecycle. Notes:
// - Only one client request (cancel or replace) can be pending at a time.
// - Fills can still arrive while a request is pending: the venue may trade the
//   order before it sees the request. If that completes the order it becomes
//   Filled, and the venue's later CancelReject/ReplaceReject is accepted as a no-op.
// clang-format off
constexpr Next kTable[kOrdStateCount][kEventCount] = {
  //                       Accept Reject      Fill          CancelReq        CancelDone  CancelRej ReplaceReq        ReplaceDone ReplaceRej VenueCancel
  /* PendingNew      */ {ToNew,  ToRejected, X,            X,               X,          X,        X,                X,          X,         X         },
  /* New             */ {X,      X,          ByQty,        ToPendingCancel, X,          X,        ToPendingReplace, X,          X,         ToCanceled},
  /* PartiallyFilled */ {X,      X,          ByQty,        ToPendingCancel, X,          X,        ToPendingReplace, X,          X,         ToCanceled},
  /* Filled          */ {X,      X,          X,            X,               X,          Same,     X,                X,          Same,      X         },
  /* PendingCancel   */ {X,      X,          FilledIfDone, X,               ToCanceled, ByQty,    X,                X,          X,         ToCanceled},
  /* Canceled        */ {X,      X,          X,            X,               X,          X,        X,                X,          X,         X         },
  /* PendingReplace  */ {X,      X,          FilledIfDone, X,               X,          X,        X,                ByQty,      ByQty,     ToCanceled},
  /* Rejected        */ {X,      X,          X,            X,               X,          X,        X,                X,          X,         X         },
};
// clang-format on

OrdState resolve(Next next, OrdState current, Qty cumQty, Qty orderQty) {
  const bool done = cumQty == orderQty;
  switch (next) {
  case Same: return current;
  case ByQty: return done ? OrdState::Filled : cumQty > 0 ? OrdState::PartiallyFilled : OrdState::New;
  case FilledIfDone: return done ? OrdState::Filled : current;
  case ToNew: return OrdState::New;
  case ToRejected: return OrdState::Rejected;
  case ToPendingCancel: return OrdState::PendingCancel;
  case ToCanceled: return OrdState::Canceled;
  case ToPendingReplace: return OrdState::PendingReplace;
  case X: break;
  }
  std::abort(); // unreachable: illegal moves are refused before resolve() is called
}

constexpr std::array<std::string_view, kEventCount> kEventNames = {
    "Accept",       "Reject",         "Fill",        "CancelRequest", "CancelDone",
    "CancelReject", "ReplaceRequest", "ReplaceDone", "ReplaceReject", "VenueCancel"};

} // namespace

ApplyResult Order::apply(const OrderEvent& event) {
  const Next next = kTable[static_cast<std::size_t>(state_)][event.index()];
  if (next == X) return ApplyResult::IllegalTransition;

  // Check quantities before changing anything, so a refused event leaves the
  // order exactly as it was.
  if (const auto* fill = std::get_if<event::Fill>(&event)) {
    if (fill->qty <= 0) return ApplyResult::InvalidQuantity;
    if (fill->qty > leavesQty()) return ApplyResult::Overfill;
    cumQty_ += fill->qty;
    fillNotional_ += static_cast<Int128>(fill->price.raw()) * fill->qty;
  } else if (const auto* replace = std::get_if<event::ReplaceDone>(&event)) {
    // A replace can shrink an order down to what's already filled, but never below it.
    if (replace->newOrderQty <= 0 || replace->newOrderQty < cumQty_) {
      return ApplyResult::InvalidQuantity;
    }
    orderQty_ = replace->newOrderQty;
    price_ = replace->newPrice;
  }
  state_ = resolve(next, state_, cumQty_, orderQty_);
  return ApplyResult::Ok;
}

Qty Order::leavesQty() const { return isTerminal(state_) ? 0 : orderQty_ - cumQty_; }

Price Order::avgPx() const {
  if (cumQty_ == 0) return Price{};
  // Round to the nearest 1/10,000, halves away from zero.
  Int128 rounded = fillNotional_ / cumQty_;
  const Int128 remainder = fillNotional_ % cumQty_;
  const Int128 twiceRemainder = remainder < 0 ? -2 * remainder : 2 * remainder;
  if (twiceRemainder >= cumQty_) rounded += fillNotional_ < 0 ? -1 : 1;
  return Price::fromRaw(static_cast<std::int64_t>(rounded));
}

bool isTerminal(OrdState state) {
  return state == OrdState::Filled || state == OrdState::Canceled || state == OrdState::Rejected;
}

std::string_view toString(OrdState state) {
  switch (state) {
  case OrdState::PendingNew: return "PendingNew";
  case OrdState::New: return "New";
  case OrdState::PartiallyFilled: return "PartiallyFilled";
  case OrdState::Filled: return "Filled";
  case OrdState::PendingCancel: return "PendingCancel";
  case OrdState::Canceled: return "Canceled";
  case OrdState::PendingReplace: return "PendingReplace";
  case OrdState::Rejected: return "Rejected";
  }
  return "?";
}

std::string_view toString(ApplyResult result) {
  switch (result) {
  case ApplyResult::Ok: return "Ok";
  case ApplyResult::IllegalTransition: return "IllegalTransition";
  case ApplyResult::InvalidQuantity: return "InvalidQuantity";
  case ApplyResult::Overfill: return "Overfill";
  }
  return "?";
}

std::string_view eventName(std::size_t eventIndex) {
  return eventIndex < kEventNames.size() ? kEventNames[eventIndex] : "?";
}

} // namespace oems
