#include "oems/order_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream>
#include <random>
#include <vector>

namespace oems {

void PrintTo(const Execution& e, std::ostream* os) {
  *os << "{resting=" << e.restingId << " " << e.qty << " @ " << formatPrice(e.price) << '}';
}

namespace {

constexpr Price px(std::int64_t cents) { return Price::fromRaw(cents * 100); } // px(1000) is 10.00

class OrderBookTest : public ::testing::Test {
protected:
  OrderBook::AddResult add(OrderId id, Side side, std::int64_t cents, Qty qty,
                           TimeInForce tif = TimeInForce::Day) {
    return book.add(id, side, px(cents), qty, tif, executions);
  }

  OrderBook book;
  std::vector<Execution> executions;
};

// --- Resting and matching --------------------------------------------------------

TEST_F(OrderBookTest, EmptyBookHasNoPrices) {
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());
  EXPECT_EQ(book.restingOrderCount(), 0U);
}

TEST_F(OrderBookTest, NonCrossingOrdersRest) {
  const auto bid = add(1, Side::Buy, 999, 100);
  const auto ask = add(2, Side::Sell, 1001, 50);
  EXPECT_EQ(bid.restingQty, 100);
  EXPECT_EQ(ask.restingQty, 50);
  EXPECT_TRUE(executions.empty());
  EXPECT_EQ(book.bestBid(), px(999));
  EXPECT_EQ(book.bestAsk(), px(1001));
  EXPECT_EQ(book.restingOrderCount(), 2U);
}

TEST_F(OrderBookTest, BestPricesTrackTheTopOfEachSide) {
  add(1, Side::Buy, 990, 10);
  add(2, Side::Buy, 995, 10);
  add(3, Side::Sell, 1010, 10);
  add(4, Side::Sell, 1005, 10);
  EXPECT_EQ(book.bestBid(), px(995));
  EXPECT_EQ(book.bestAsk(), px(1005));
}

TEST_F(OrderBookTest, TradesAtTheRestingPrice) {
  add(1, Side::Sell, 1000, 100);
  const auto result = add(2, Side::Buy, 1005, 100); // willing to pay more than the ask
  EXPECT_EQ(result.filledQty, 100);
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1000), 100}})); // gets the better price
  EXPECT_EQ(book.restingOrderCount(), 0U);
}

TEST_F(OrderBookTest, SamePriceTradesInArrivalOrder) {
  add(1, Side::Sell, 1000, 30);
  add(2, Side::Sell, 1000, 30);
  add(3, Side::Sell, 1000, 30);
  add(4, Side::Buy, 1000, 50);
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1000), 30}, {2, px(1000), 20}}));
  EXPECT_EQ(book.openQty(2), 10); // partly filled, still at the front
  EXPECT_EQ(book.openQty(3), 30);
}

TEST_F(OrderBookTest, SweepsPriceLevelsBestFirst) {
  add(1, Side::Sell, 1002, 10);
  add(2, Side::Sell, 1000, 10);
  add(3, Side::Sell, 1001, 10);
  const auto result = add(4, Side::Buy, 1001, 25);
  EXPECT_EQ(executions, (std::vector<Execution>{{2, px(1000), 10}, {3, px(1001), 10}}));
  EXPECT_EQ(result.filledQty, 20);
  EXPECT_EQ(result.restingQty, 5); // the rest waits at its own limit, not past it
  EXPECT_EQ(book.bestBid(), px(1001));
  EXPECT_EQ(book.bestAsk(), px(1002));
}

TEST_F(OrderBookTest, SellSideMatchesHighestBidFirst) {
  add(1, Side::Buy, 998, 10);
  add(2, Side::Buy, 1000, 10);
  add(3, Side::Sell, 997, 15);
  EXPECT_EQ(executions, (std::vector<Execution>{{2, px(1000), 10}, {1, px(998), 5}}));
}

TEST_F(OrderBookTest, PartiallyFilledRestingOrderKeepsItsPlace) {
  add(1, Side::Sell, 1000, 50);
  add(2, Side::Sell, 1000, 50);
  add(3, Side::Buy, 1000, 20);
  add(4, Side::Buy, 1000, 20);
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1000), 20}})); // order 1 again, not 2
}

TEST_F(OrderBookTest, IocRemainderIsCancelledNotRested) {
  add(1, Side::Sell, 1000, 30);
  const auto result = add(2, Side::Buy, 1000, 50, TimeInForce::Ioc);
  EXPECT_EQ(result.filledQty, 30);
  EXPECT_EQ(result.cancelledQty, 20);
  EXPECT_EQ(result.restingQty, 0);
  EXPECT_FALSE(book.bestBid().has_value());
}

TEST_F(OrderBookTest, IocThatCannotTradeIsCancelledEntirely) {
  add(1, Side::Sell, 1010, 30);
  const auto result = add(2, Side::Buy, 1000, 50, TimeInForce::Ioc);
  EXPECT_EQ(result.cancelledQty, 50);
  EXPECT_TRUE(executions.empty());
  EXPECT_EQ(book.restingOrderCount(), 1U);
}

TEST_F(OrderBookTest, DepthSumsOneLevel) {
  add(1, Side::Buy, 1000, 10);
  add(2, Side::Buy, 1000, 15);
  add(3, Side::Buy, 999, 7);
  EXPECT_EQ(book.depthAt(Side::Buy, px(1000)), 25);
  EXPECT_EQ(book.depthAt(Side::Buy, px(999)), 7);
  EXPECT_EQ(book.depthAt(Side::Sell, px(1000)), 0);
}

// --- Cancel ------------------------------------------------------------------------

TEST_F(OrderBookTest, CancelRemovesRestingOrderAndEmptyLevel) {
  add(1, Side::Buy, 1000, 10);
  add(2, Side::Buy, 999, 10);
  EXPECT_TRUE(book.cancel(1));
  EXPECT_EQ(book.bestBid(), px(999));
  EXPECT_FALSE(book.contains(1));
}

TEST_F(OrderBookTest, CancelOfUnknownFilledOrAlreadyCancelledOrderFails) {
  EXPECT_FALSE(book.cancel(42)); // unknown

  add(1, Side::Sell, 1000, 10);
  add(2, Side::Buy, 1000, 10); // fills order 1
  EXPECT_FALSE(book.cancel(1));

  add(3, Side::Buy, 990, 10);
  EXPECT_TRUE(book.cancel(3));
  EXPECT_FALSE(book.cancel(3));
}

TEST_F(OrderBookTest, CancelInTheMiddleOfAQueueKeepsTheOthersInOrder) {
  add(1, Side::Sell, 1000, 10);
  add(2, Side::Sell, 1000, 10);
  add(3, Side::Sell, 1000, 10);
  ASSERT_TRUE(book.cancel(2));
  add(4, Side::Buy, 1000, 20);
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1000), 10}, {3, px(1000), 10}}));
}

// --- Replace -----------------------------------------------------------------------

TEST_F(OrderBookTest, ReplaceDownAtSamePriceKeepsPriority) {
  add(1, Side::Sell, 1000, 50);
  add(2, Side::Sell, 1000, 50);
  ASSERT_TRUE(book.replace(1, px(1000), 30, executions));
  add(3, Side::Buy, 1000, 30);
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1000), 30}}));
}

TEST_F(OrderBookTest, ReplaceUpLosesPriority) {
  add(1, Side::Sell, 1000, 50);
  add(2, Side::Sell, 1000, 50);
  ASSERT_TRUE(book.replace(1, px(1000), 60, executions));
  add(3, Side::Buy, 1000, 50);
  EXPECT_EQ(executions, (std::vector<Execution>{{2, px(1000), 50}})); // order 1 went to the back
  EXPECT_EQ(book.openQty(1), 60);
}

TEST_F(OrderBookTest, ReplacePriceLosesPriorityAndCanTradeAtOnce) {
  add(1, Side::Sell, 1005, 40);
  add(2, Side::Buy, 1000, 50);
  ASSERT_TRUE(book.replace(2, px(1005), 50, executions)); // now crosses the ask
  EXPECT_EQ(executions, (std::vector<Execution>{{1, px(1005), 40}}));
  EXPECT_EQ(book.openQty(2), 10);
  EXPECT_EQ(book.bestBid(), px(1005));
}

TEST_F(OrderBookTest, ReplaceToZeroRemovesTheOrder) {
  add(1, Side::Buy, 1000, 50);
  ASSERT_TRUE(book.replace(1, px(1000), 0, executions));
  EXPECT_FALSE(book.contains(1));
  EXPECT_FALSE(book.bestBid().has_value());
}

TEST_F(OrderBookTest, ReplaceOfOrderNotRestingFails) {
  EXPECT_FALSE(book.replace(7, px(1000), 10, executions));
  EXPECT_EQ(book.restingOrderCount(), 0U);
}

// --- Against a reference model ------------------------------------------------------

// A deliberately simple book: a flat list, scanned in full for every match. Far
// too slow for real use, but easy to check by eye, which is the point.
class ReferenceBook {
public:
  std::vector<Execution> add(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif) {
    std::vector<Execution> out;
    Qty left = qty;
    while (left > 0) {
      auto best = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->side == side) continue;
        const bool crosses = side == Side::Buy ? it->price <= limit : it->price >= limit;
        if (!crosses) continue;
        if (best == entries_.end() || better(*it, *best)) best = it;
      }
      if (best == entries_.end()) break;
      const Qty traded = std::min(left, best->open);
      out.push_back({best->id, best->price, traded});
      left -= traded;
      best->open -= traded;
      if (best->open == 0) entries_.erase(best);
    }
    if (left > 0 && tif == TimeInForce::Day) entries_.push_back({id, side, limit, left, nextSeq_++});
    return out;
  }

  bool cancel(OrderId id) {
    const auto it = find(id);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
  }

  std::optional<std::vector<Execution>> replace(OrderId id, Price newPrice, Qty newOpen) {
    const auto it = find(id);
    if (it == entries_.end()) return std::nullopt;
    if (newOpen > 0 && newPrice == it->price && newOpen <= it->open) {
      it->open = newOpen;
      return std::vector<Execution>{};
    }
    const Side side = it->side;
    entries_.erase(it);
    if (newOpen == 0) return std::vector<Execution>{};
    return add(id, side, newPrice, newOpen, TimeInForce::Day);
  }

  std::optional<Price> best(Side side) const {
    std::optional<Price> result;
    for (const Entry& e : entries_) {
      if (e.side != side) continue;
      if (!result || (side == Side::Buy ? e.price > *result : e.price < *result)) result = e.price;
    }
    return result;
  }

  std::size_t size() const { return entries_.size(); }

  // Calls f(id, openQty) for every resting order.
  template <typename F> void forEach(F f) const {
    for (const Entry& e : entries_) f(e.id, e.open);
  }

  OrderId randomRestingId(std::mt19937& rng) const {
    return entries_[std::uniform_int_distribution<std::size_t>(0, entries_.size() - 1)(rng)].id;
  }

private:
  struct Entry {
    OrderId id;
    Side side;
    Price price;
    Qty open;
    std::uint64_t seq; // arrival order
  };
  // For the side being matched against: better price first, then earlier arrival.
  static bool better(const Entry& a, const Entry& b) {
    if (a.price != b.price) return a.side == Side::Sell ? a.price < b.price : a.price > b.price;
    return a.seq < b.seq;
  }
  std::vector<Entry>::iterator find(OrderId id) {
    return std::find_if(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; });
  }

  std::vector<Entry> entries_;
  std::uint64_t nextSeq_ = 0;
};

// 200,000 random adds, cancels and replaces, in a narrow price range so orders
// cross often. After every operation both books must report the same trades,
// the same best prices and the same number of resting orders; every 100
// operations, the same open quantity for every resting order.
TEST(OrderBookModel, MatchesReferenceBookOnRandomFlow) {
  std::mt19937 rng(20261011); // fixed seed: the same flow every run
  std::uniform_int_distribution<int> op(0, 9);
  std::uniform_int_distribution<std::int64_t> cents(990, 1010);
  std::uniform_int_distribution<Qty> qty(1, 100);
  std::uniform_int_distribution<int> coin(0, 4);

  OrderBook book;
  ReferenceBook reference;
  std::vector<Execution> executions;
  OrderId nextId = 1;

  for (int step = 0; step < 200'000; ++step) {
    const int choice = op(rng);
    const bool full = reference.size() > 300; // keep the book small enough to scan quickly
    if (!full && (choice < 6 || nextId == 1)) {
      const OrderId id = nextId++;
      const Side side = coin(rng) < 2 ? Side::Buy : Side::Sell;
      const Price limit = px(cents(rng));
      const Qty size = qty(rng);
      const TimeInForce tif = coin(rng) == 0 ? TimeInForce::Ioc : TimeInForce::Day;
      book.add(id, side, limit, size, tif, executions);
      ASSERT_EQ(executions, reference.add(id, side, limit, size, tif)) << "step " << step;
    } else {
      // Mostly an order that's resting now; sometimes any ID at all, so unknown,
      // filled and cancelled orders are covered too.
      const OrderId id = reference.size() > 0 && coin(rng) != 0
                             ? reference.randomRestingId(rng)
                             : std::uniform_int_distribution<OrderId>(1, nextId)(rng);
      if (full || choice < 8) {
        ASSERT_EQ(book.cancel(id), reference.cancel(id)) << "step " << step;
      } else {
        const Price newPrice = px(cents(rng));
        const Qty newOpen = coin(rng) == 0 ? 0 : qty(rng);
        const bool replaced = book.replace(id, newPrice, newOpen, executions);
        const auto expected = reference.replace(id, newPrice, newOpen);
        ASSERT_EQ(replaced, expected.has_value()) << "step " << step;
        if (expected) {
          ASSERT_EQ(executions, *expected) << "step " << step;
        }
      }
    }
    ASSERT_EQ(book.bestBid(), reference.best(Side::Buy)) << "step " << step;
    ASSERT_EQ(book.bestAsk(), reference.best(Side::Sell)) << "step " << step;
    ASSERT_EQ(book.restingOrderCount(), reference.size()) << "step " << step;
    if (book.bestBid() && book.bestAsk()) {
      ASSERT_LT(*book.bestBid(), *book.bestAsk()) << "crossed book at step " << step;
    }
    if (step % 100 == 0) {
      // Same count plus same open quantity for each reference order means the
      // two books hold exactly the same orders.
      reference.forEach([&](OrderId id, Qty open) { EXPECT_EQ(book.openQty(id), open) << "order " << id; });
      if (HasFailure()) FAIL() << "open quantities differ at step " << step;
    }
  }
}

} // namespace
} // namespace oems
