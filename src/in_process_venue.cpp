#include "oems/in_process_venue.hpp"

#include <cassert>

namespace oems {

void InProcessVenue::submit(const NewOrderRequest& request) {
  assert(!reporting_ && "VenueListener must not call back into the venue");
  if (request.qty <= 0 || request.price.raw() <= 0 || resting_.contains(request.id)) {
    report(request.id, event::Reject{});
    return;
  }
  report(request.id, event::Accept{});

  OrderBook& book = books_[request.symbol];
  const OrderBook::AddResult result =
      book.add(request.id, request.side, request.price, request.qty, request.tif, executions_);
  const Qty filled = reportFills(request.id);
  if (result.restingQty > 0) resting_.emplace(request.id, Resting{request.symbol, request.qty, filled});
  if (result.cancelledQty > 0) report(request.id, event::VenueCancel{});
}

void InProcessVenue::cancel(OrderId id) {
  assert(!reporting_ && "VenueListener must not call back into the venue");
  const auto found = resting_.find(id);
  if (found == resting_.end() || !books_[found->second.symbol].cancel(id)) {
    report(id, event::CancelReject{});
    return;
  }
  resting_.erase(found);
  report(id, event::CancelDone{});
}

void InProcessVenue::replace(OrderId id, Qty newOrderQty, Price newPrice) {
  assert(!reporting_ && "VenueListener must not call back into the venue");
  const auto found = resting_.find(id);
  if (found == resting_.end() || newOrderQty < found->second.cumQty || newOrderQty <= 0 ||
      newPrice.raw() <= 0) {
    report(id, event::ReplaceReject{});
    return;
  }

  Resting& order = found->second;
  OrderBook& book = books_[order.symbol];
  [[maybe_unused]] const bool wasResting = book.replace(id, newPrice, newOrderQty - order.cumQty, executions_);
  assert(wasResting); // resting_ and the books always agree
  order.orderQty = newOrderQty;
  report(id, event::ReplaceDone{newOrderQty, newPrice});
  order.cumQty += reportFills(id); // a new price can trade straight away
  if (!book.contains(id)) resting_.erase(id); // filled, or replaced down to what had filled
}

const OrderBook* InProcessVenue::book(SymbolId symbol) const {
  const auto found = books_.find(symbol);
  return found == books_.end() ? nullptr : &found->second;
}

void InProcessVenue::report(OrderId id, const OrderEvent& event) {
  reporting_ = true;
  listener_.onVenueEvent(id, event);
  reporting_ = false;
}

Qty InProcessVenue::reportFills(OrderId aggressor) {
  Qty aggressorFilled = 0;
  for (const Execution& execution : executions_) {
    report(aggressor, event::Fill{execution.qty, execution.price});
    report(execution.restingId, event::Fill{execution.qty, execution.price});
    aggressorFilled += execution.qty;

    // Every order on a book was put there by submit() or replace(), so it's tracked here.
    const auto resting = resting_.find(execution.restingId);
    assert(resting != resting_.end());
    resting->second.cumQty += execution.qty;
    if (resting->second.cumQty == resting->second.orderQty) resting_.erase(resting); // fully filled
  }
  return aggressorFilled;
}

} // namespace oems
