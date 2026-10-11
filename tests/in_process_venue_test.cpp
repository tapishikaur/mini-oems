#include "oems/in_process_venue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oems {
namespace {

constexpr Price px(std::int64_t cents) { return Price::fromRaw(cents * 100); } // px(1000) is 10.00
constexpr SymbolId kSymbol = 7;

std::string describe(OrderId id, const OrderEvent& e) {
  std::string text = std::to_string(id) + " " + std::string(eventName(e.index()));
  if (const auto* fill = std::get_if<event::Fill>(&e)) {
    text += " " + std::to_string(fill->qty) + "@" + formatPrice(fill->price);
  } else if (const auto* replace = std::get_if<event::ReplaceDone>(&e)) {
    text += " " + std::to_string(replace->newOrderQty) + "@" + formatPrice(replace->newPrice);
  }
  return text;
}

// Records every event as text, e.g. "2 Fill 30@10".
class Recorder : public VenueListener {
public:
  void onVenueEvent(OrderId id, const OrderEvent& e) override { events.push_back(describe(id, e)); }
  std::vector<std::string> events;
};

class InProcessVenueTest : public ::testing::Test {
protected:
  void submit(OrderId id, Side side, std::int64_t cents, Qty qty, TimeInForce tif = TimeInForce::Day) {
    venue.submit({id, kSymbol, side, px(cents), qty, tif});
  }
  std::vector<std::string> take() { return std::exchange(recorder.events, {}); }

  Recorder recorder;
  InProcessVenue venue{recorder};
};

using Events = std::vector<std::string>;

TEST_F(InProcessVenueTest, RestingOrderIsAccepted) {
  submit(1, Side::Buy, 1000, 100);
  EXPECT_EQ(take(), (Events{"1 Accept"}));
  EXPECT_EQ(venue.book(kSymbol)->bestBid(), px(1000));
}

TEST_F(InProcessVenueTest, TradeIsReportedToBothOrders) {
  submit(1, Side::Sell, 1000, 30);
  submit(2, Side::Buy, 1005, 50);
  EXPECT_EQ(take(), (Events{"1 Accept", "2 Accept", "2 Fill 30@10", "1 Fill 30@10"}));
  EXPECT_EQ(venue.book(kSymbol)->openQty(2), 20); // the rest of order 2 now rests
}

TEST_F(InProcessVenueTest, UnfilledRestOfIocIsCancelledByVenue) {
  submit(1, Side::Sell, 1000, 30);
  submit(2, Side::Buy, 1000, 50, TimeInForce::Ioc);
  EXPECT_EQ(take(), (Events{"1 Accept", "2 Accept", "2 Fill 30@10", "1 Fill 30@10", "2 VenueCancel"}));
}

TEST_F(InProcessVenueTest, InvalidOrdersAreRejected) {
  submit(1, Side::Buy, 1000, 0);
  submit(2, Side::Buy, 0, 10);
  submit(3, Side::Buy, 1000, 10);
  submit(3, Side::Buy, 1000, 10); // same ID as a resting order
  EXPECT_EQ(take(), (Events{"1 Reject", "2 Reject", "3 Accept", "3 Reject"}));
}

TEST_F(InProcessVenueTest, CancelOnlySucceedsForRestingOrders) {
  submit(1, Side::Buy, 1000, 10);
  submit(2, Side::Sell, 1010, 10);
  submit(3, Side::Buy, 1010, 10); // fills order 2
  take();

  venue.cancel(1);
  venue.cancel(1);  // already cancelled
  venue.cancel(2);  // already filled
  venue.cancel(99); // never existed
  EXPECT_EQ(take(), (Events{"1 CancelDone", "1 CancelReject", "2 CancelReject", "99 CancelReject"}));
  EXPECT_FALSE(venue.book(kSymbol)->bestBid().has_value());
}

TEST_F(InProcessVenueTest, ReplaceChangesTheRestingOrder) {
  submit(1, Side::Buy, 1000, 100);
  take();
  venue.replace(1, 60, px(999));
  EXPECT_EQ(take(), (Events{"1 ReplaceDone 60@9.99"}));
  EXPECT_EQ(venue.book(kSymbol)->openQty(1), 60);
  EXPECT_EQ(venue.book(kSymbol)->bestBid(), px(999));
}

TEST_F(InProcessVenueTest, ReplaceToCrossingPriceTradesAtOnce) {
  submit(1, Side::Sell, 1005, 40);
  submit(2, Side::Buy, 1000, 50);
  take();
  venue.replace(2, 50, px(1005));
  EXPECT_EQ(take(), (Events{"2 ReplaceDone 50@10.05", "2 Fill 40@10.05", "1 Fill 40@10.05"}));
  EXPECT_EQ(venue.book(kSymbol)->openQty(2), 10);
}

TEST_F(InProcessVenueTest, ReplaceCountsWhatHasAlreadyFilled) {
  submit(1, Side::Buy, 1000, 100);
  submit(2, Side::Sell, 1000, 40); // fills 40 of order 1
  take();

  venue.replace(1, 30, px(1000)); // below the 40 filled
  EXPECT_EQ(take(), (Events{"1 ReplaceReject"}));

  venue.replace(1, 70, px(1000)); // 70 total, so 30 still open
  EXPECT_EQ(take(), (Events{"1 ReplaceDone 70@10"}));
  EXPECT_EQ(venue.book(kSymbol)->openQty(1), 30);

  venue.replace(1, 40, px(1000)); // down to exactly what filled: the order is done
  EXPECT_EQ(take(), (Events{"1 ReplaceDone 40@10"}));
  EXPECT_FALSE(venue.book(kSymbol)->contains(1));
  venue.cancel(1);
  EXPECT_EQ(take(), (Events{"1 CancelReject"}));
}

TEST_F(InProcessVenueTest, ReplaceOfUnknownOrderIsRejected) {
  venue.replace(5, 10, px(1000));
  EXPECT_EQ(take(), (Events{"5 ReplaceReject"}));
}

TEST_F(InProcessVenueTest, SymbolsHaveSeparateBooks) {
  venue.submit({1, 1, Side::Sell, px(1000), 10, TimeInForce::Day});
  venue.submit({2, 2, Side::Buy, px(1000), 10, TimeInForce::Day}); // other symbol: no trade
  EXPECT_EQ(take(), (Events{"1 Accept", "2 Accept"}));
  EXPECT_EQ(venue.book(1)->bestAsk(), px(1000));
  EXPECT_EQ(venue.book(2)->bestBid(), px(1000));
  EXPECT_EQ(venue.book(3), nullptr);
}

// --- Venue and state machine together -------------------------------------------------

// A minimal order manager: keeps an Order per ID and applies every venue event
// to it through the Phase 1 state machine.
class OrderTracker : public VenueListener {
public:
  void onVenueEvent(OrderId id, const OrderEvent& e) override {
    const auto found = orders.find(id);
    if (found == orders.end()) {
      ADD_FAILURE() << "event for unknown order: " << describe(id, e);
      return;
    }
    const ApplyResult result = found->second.apply(e);
    if (result != ApplyResult::Ok) {
      ADD_FAILURE() << describe(id, e) << " in state " << toString(found->second.state()) << ": "
                    << toString(result);
    }
    if (const auto* fill = std::get_if<event::Fill>(&e)) {
      (found->second.side() == Side::Buy ? bought : sold) += fill->qty;
    }
  }

  std::unordered_map<OrderId, Order> orders;
  Qty bought = 0;
  Qty sold = 0;
};

// 30,000 random submits, cancels and replaces across two symbols. Every venue
// event must be a legal transition for the order it's about; every trade adds
// the same quantity to the buy and sell sides; and the books must agree with
// the orders' open quantities.
TEST(VenueWithStateMachine, RandomFlowStaysConsistent) {
  std::mt19937 rng(20261011);
  std::uniform_int_distribution<int> op(0, 9);
  std::uniform_int_distribution<std::int64_t> cents(990, 1010);
  std::uniform_int_distribution<Qty> qty(1, 100);
  std::uniform_int_distribution<int> coin(0, 4);

  OrderTracker tracker;
  InProcessVenue venue(tracker);
  std::vector<OrderId> live; // orders that may still be resting
  OrderId nextId = 1;

  for (int step = 0; step < 30'000; ++step) {
    const int choice = op(rng);
    if (live.empty() || choice < 6) {
      const OrderId id = nextId++;
      const SymbolId symbol = static_cast<SymbolId>(coin(rng) % 2);
      const Side side = coin(rng) < 2 ? Side::Buy : Side::Sell;
      const Price limit = px(cents(rng));
      const Qty size = qty(rng);
      const TimeInForce tif = coin(rng) == 0 ? TimeInForce::Ioc : TimeInForce::Day;
      tracker.orders.emplace(id, Order(id, symbol, 0, side, limit, size));
      venue.submit({id, symbol, side, limit, size, tif});
      live.push_back(id);
    } else {
      const std::size_t pick = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
      Order& order = tracker.orders.at(live[pick]);
      if (isTerminal(order.state())) {
        live[pick] = live.back(); // forget orders that are done
        live.pop_back();
        continue;
      }
      if (choice < 8) {
        ASSERT_EQ(order.apply(event::CancelRequest{}), ApplyResult::Ok);
        venue.cancel(order.id());
        ASSERT_EQ(order.state(), OrdState::Canceled); // a live order is always resting
      } else {
        ASSERT_EQ(order.apply(event::ReplaceRequest{}), ApplyResult::Ok);
        venue.replace(order.id(), qty(rng) + 20, px(cents(rng)));
        ASSERT_NE(order.state(), OrdState::PendingReplace); // answered either way
      }
    }
    ASSERT_FALSE(::testing::Test::HasFailure()) << "step " << step;
    ASSERT_EQ(tracker.bought, tracker.sold) << "step " << step;

    if (step % 500 == 0) {
      for (const auto& [id, order] : tracker.orders) {
        const Qty onBook = venue.book(order.symbol())->openQty(id);
        ASSERT_EQ(onBook, order.leavesQty()) << "order " << id << " step " << step;
      }
    }
  }
}

} // namespace
} // namespace oems
