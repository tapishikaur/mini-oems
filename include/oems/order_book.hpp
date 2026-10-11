#pragma once
// A limit order book for one symbol, with price-time priority: the best price
// trades first, and at the same price, whoever arrived first. A trade happens at
// the resting order's price, so an aggressive order can get a better price than
// its limit but never a worse one.
//
// Data structures: one sorted map of price levels per side, each level a FIFO
// list, plus a hash index from order ID to its position. Every operation is
// O(log levels) or better, and cancel/replace never search a level. These are
// node-based containers chosen for clarity; a flat, preallocated layout would
// be faster, but the Phase 5 benchmark shows whether the book is worth it.

#include "oems/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oems {

// One match between an incoming order and a resting one.
struct Execution {
  OrderId restingId;
  Price price; // always the resting order's price
  Qty qty;
  bool operator==(const Execution&) const = default;
};

class OrderBook {
public:
  struct AddResult {
    Qty filledQty = 0;    // traded on arrival
    Qty restingQty = 0;   // left on the book
    Qty cancelledQty = 0; // unfilled IOC remainder, never rested
  };

  // Matches an incoming order, then rests or cancels whatever is left.
  // `executions` is cleared, then gets one entry per match. Reusing the same
  // vector across calls means no memory is allocated per order once it has grown.
  // Precondition: `id` isn't already resting; IDs are assigned by the caller.
  AddResult add(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif,
                std::vector<Execution>& executions);

  // Removes a resting order. Returns false if it isn't resting: unknown,
  // already filled, or already cancelled.
  bool cancel(OrderId id);

  // Changes a resting order's price and/or open quantity, following the usual
  // exchange rule. Lowering the quantity at the same price keeps the order's
  // place in the queue. Any other change sends it to the back of the queue, and
  // a new price may trade straight away. newOpenQty == 0 removes the order.
  // Returns false, changing nothing, if the order isn't resting.
  bool replace(OrderId id, Price newPrice, Qty newOpenQty, std::vector<Execution>& executions);

  std::optional<Price> bestBid() const;
  std::optional<Price> bestAsk() const;
  Qty depthAt(Side side, Price price) const; // total open quantity at one price level
  Qty openQty(OrderId id) const;             // 0 if the order isn't resting
  bool contains(OrderId id) const { return index_.contains(id); }
  std::size_t restingOrderCount() const { return index_.size(); }

private:
  struct Resting {
    OrderId id;
    Qty openQty;
  };
  using Level = std::list<Resting>; // the front trades first
  using Bids = std::map<Price, Level, std::greater<>>; // highest price first
  using Asks = std::map<Price, Level, std::less<>>;    // lowest price first
  struct Location {
    Side side;
    Price price;
    Level::iterator position;
  };

  template <typename Opposite>
  Qty match(Opposite& opposite, Side side, Price limit, Qty qty, std::vector<Execution>& executions);
  void rest(OrderId id, Side side, Price price, Qty qty);
  void unlink(const Location& location); // removes from its level, not from the index

  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Location> index_;
};

} // namespace oems
