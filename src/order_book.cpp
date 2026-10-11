#include "oems/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace oems {
namespace {

bool crosses(Side incoming, Price levelPrice, Price limit) {
  return incoming == Side::Buy ? levelPrice <= limit : levelPrice >= limit;
}

template <typename Levels, typename Position>
void eraseFromLevel(Levels& levels, Price price, Position position) {
  const auto level = levels.find(price);
  level->second.erase(position);
  if (level->second.empty()) levels.erase(level); // no empty levels, so begin() is always the best price
}

template <typename Levels>
Qty totalAt(const Levels& levels, Price price) {
  const auto level = levels.find(price);
  if (level == levels.end()) return 0;
  Qty total = 0;
  for (const auto& resting : level->second) total += resting.openQty;
  return total;
}

} // namespace

OrderBook::AddResult OrderBook::add(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif,
                                    std::vector<Execution>& executions) {
  assert(!index_.contains(id) && "order IDs must be unique");
  executions.clear();

  AddResult result;
  result.filledQty = side == Side::Buy ? match(asks_, side, limit, qty, executions)
                                       : match(bids_, side, limit, qty, executions);
  const Qty left = qty - result.filledQty;
  if (left > 0) {
    if (tif == TimeInForce::Day) {
      rest(id, side, limit, left);
      result.restingQty = left;
    } else {
      result.cancelledQty = left;
    }
  }
  return result;
}

bool OrderBook::cancel(OrderId id) {
  const auto found = index_.find(id);
  if (found == index_.end()) return false;
  unlink(found->second);
  index_.erase(found);
  return true;
}

bool OrderBook::replace(OrderId id, Price newPrice, Qty newOpenQty, std::vector<Execution>& executions) {
  executions.clear();
  const auto found = index_.find(id);
  if (found == index_.end()) return false;

  const Location location = found->second;
  if (newOpenQty > 0 && newPrice == location.price && newOpenQty <= location.position->openQty) {
    location.position->openQty = newOpenQty; // smaller, same price: keeps its place in the queue
    return true;
  }

  unlink(location);
  index_.erase(found);
  if (newOpenQty > 0) {
    add(id, location.side, newPrice, newOpenQty, TimeInForce::Day, executions); // back of the queue
  }
  return true;
}

std::optional<Price> OrderBook::bestBid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

Qty OrderBook::depthAt(Side side, Price price) const {
  return side == Side::Buy ? totalAt(bids_, price) : totalAt(asks_, price);
}

Qty OrderBook::openQty(OrderId id) const {
  const auto found = index_.find(id);
  return found == index_.end() ? 0 : found->second.position->openQty;
}

template <typename Opposite>
Qty OrderBook::match(Opposite& opposite, Side side, Price limit, Qty qty,
                     std::vector<Execution>& executions) {
  Qty filled = 0;
  while (filled < qty && !opposite.empty()) {
    const auto level = opposite.begin(); // best price on the other side
    const Price levelPrice = level->first;
    if (!crosses(side, levelPrice, limit)) break;

    Level& queue = level->second;
    while (filled < qty && !queue.empty()) {
      Resting& resting = queue.front(); // earliest arrival at this price
      const Qty traded = std::min(qty - filled, resting.openQty);
      executions.push_back({resting.id, levelPrice, traded});
      filled += traded;
      resting.openQty -= traded;
      if (resting.openQty == 0) {
        index_.erase(resting.id);
        queue.pop_front();
      }
    }
    if (queue.empty()) opposite.erase(level);
  }
  return filled;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  Level& queue = side == Side::Buy ? bids_[price] : asks_[price];
  queue.push_back({id, qty});
  index_.emplace(id, Location{side, price, std::prev(queue.end())});
}

void OrderBook::unlink(const Location& location) {
  if (location.side == Side::Buy) {
    eraseFromLevel(bids_, location.price, location.position);
  } else {
    eraseFromLevel(asks_, location.price, location.position);
  }
}

} // namespace oems
