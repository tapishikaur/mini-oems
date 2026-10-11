#pragma once
// A venue that runs inside the process: one OrderBook per symbol. It reports
// every change synchronously, before submit()/cancel()/replace() return, which
// keeps the whole system single-threaded and deterministic.

#include "oems/order_book.hpp"
#include "oems/venue.hpp"

#include <unordered_map>
#include <vector>

namespace oems {

class InProcessVenue final : public IOrderRouter {
public:
  // The listener must not call back into the venue from onVenueEvent().
  explicit InProcessVenue(VenueListener& listener) : listener_(listener) {}

  // Event order. A new order gets Accept, then for each match one Fill to it
  // and one to the resting order, then VenueCancel for any unfilled IOC rest.
  // Orders with a non-positive quantity or price, or an ID the venue already
  // holds, get Reject.
  void submit(const NewOrderRequest& request) override;
  // CancelDone if the order was resting, otherwise CancelReject.
  void cancel(OrderId id) override;
  // ReplaceDone then any Fills the new price causes, or ReplaceReject if the order
  // isn't resting or newOrderQty is below what has already filled.
  void replace(OrderId id, Qty newOrderQty, Price newPrice) override;

  const OrderBook* book(SymbolId symbol) const; // nullptr if no order was ever sent for it

private:
  // What the venue remembers about an order resting on one of its books.
  struct Resting {
    SymbolId symbol;
    Qty orderQty;
    Qty cumQty;
  };

  void report(OrderId id, const OrderEvent& event);
  Qty reportFills(OrderId aggressor); // reports executions_; returns the aggressor's filled qty

  VenueListener& listener_;
  std::unordered_map<SymbolId, OrderBook> books_;
  std::unordered_map<OrderId, Resting> resting_;
  std::vector<Execution> executions_; // reused by every call, so matching doesn't allocate
  bool reporting_ = false;            // guards against the listener calling back in
};

} // namespace oems
