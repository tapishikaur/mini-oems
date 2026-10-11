#pragma once
// The boundary between order handling and execution. The order manager sends
// requests through IOrderRouter and hears back through VenueListener, as
// OrderEvents it applies to its orders. Here the venue is the in-process order
// book (InProcessVenue); in a real system it would be an exchange or broker
// connection behind the same interface.

#include "oems/order.hpp"
#include "oems/types.hpp"

namespace oems {

struct NewOrderRequest {
  OrderId id;
  SymbolId symbol;
  Side side;
  Price price;
  Qty qty;
  TimeInForce tif;
};

class VenueListener {
public:
  virtual ~VenueListener() = default;
  // One call per change to an order, in the order the changes happened.
  virtual void onVenueEvent(OrderId id, const OrderEvent& event) = 0;
};

class IOrderRouter {
public:
  virtual ~IOrderRouter() = default;
  virtual void submit(const NewOrderRequest& request) = 0;
  virtual void cancel(OrderId id) = 0;
  // newOrderQty is the order's new total quantity, including anything already filled.
  virtual void replace(OrderId id, Qty newOrderQty, Price newPrice) = 0;
};

} // namespace oems
