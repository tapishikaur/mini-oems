#include "oems/order.hpp"
#include "printers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <map>
#include <ostream>
#include <random>
#include <string>
#include <tuple>
#include <utility>

namespace oems {

namespace {

using event::Accept;
using event::CancelDone;
using event::CancelReject;
using event::CancelRequest;
using event::Fill;
using event::Reject;
using event::ReplaceDone;
using event::ReplaceReject;
using event::ReplaceRequest;
using event::VenueCancel;

const Price kPx = Price::fromUnits(10);

constexpr std::array<OrdState, kOrdStateCount> kAllStates = {
    OrdState::PendingNew, OrdState::New,      OrdState::PartiallyFilled, OrdState::Filled,
    OrdState::PendingCancel, OrdState::Canceled, OrdState::PendingReplace, OrdState::Rejected};

template <typename E> constexpr std::size_t column() { return OrderEvent(E{}).index(); }

// One sample of each event. The quantities never complete or overfill the
// orders built by orderIn().
OrderEvent sampleEvent(std::size_t column) {
  static_assert(kEventCount == 10, "add the new event here and to legalMoves()");
  switch (column) {
  case 0: return Accept{};
  case 1: return Reject{};
  case 2: return Fill{10, kPx};
  case 3: return CancelRequest{};
  case 4: return CancelDone{};
  case 5: return CancelReject{};
  case 6: return ReplaceRequest{};
  case 7: return ReplaceDone{80, Price::fromRaw(105'000)};
  case 8: return ReplaceReject{};
  default: return VenueCancel{};
  }
}

// An order for 100 @ 10.00, driven into `state` through legal events only, so
// every test starts from a state the machine can really reach. Live states that
// allow fills start with 40 already filled.
Order orderIn(OrdState state) {
  Order order(1, 0, 0, Side::Buy, kPx, 100);
  const auto step = [&order](const OrderEvent& e) {
    const ApplyResult r = order.apply(e);
    if (r != ApplyResult::Ok) ADD_FAILURE() << "setup " << eventName(e.index()) << ": " << toString(r);
  };
  switch (state) {
  case OrdState::PendingNew: break;
  case OrdState::New: step(Accept{}); break;
  case OrdState::PartiallyFilled: step(Accept{}); step(Fill{40, kPx}); break;
  case OrdState::Filled: step(Accept{}); step(Fill{100, kPx}); break;
  case OrdState::PendingCancel: step(Accept{}); step(Fill{40, kPx}); step(CancelRequest{}); break;
  case OrdState::Canceled: step(Accept{}); step(CancelRequest{}); step(CancelDone{}); break;
  case OrdState::PendingReplace: step(Accept{}); step(Fill{40, kPx}); step(ReplaceRequest{}); break;
  case OrdState::Rejected: step(Reject{}); break;
  }
  EXPECT_EQ(order.state(), state);
  return order;
}

// The legal moves for the sample events, written out independently of the table
// in order.cpp. Every (state, event) pair not listed here must be refused.
const std::map<std::pair<OrdState, std::size_t>, OrdState>& legalMoves() {
  using S = OrdState;
  static const std::map<std::pair<S, std::size_t>, S> moves = {
      {{S::PendingNew, column<Accept>()}, S::New},
      {{S::PendingNew, column<Reject>()}, S::Rejected},

      {{S::New, column<Fill>()}, S::PartiallyFilled},
      {{S::New, column<CancelRequest>()}, S::PendingCancel},
      {{S::New, column<ReplaceRequest>()}, S::PendingReplace},
      {{S::New, column<VenueCancel>()}, S::Canceled},

      {{S::PartiallyFilled, column<Fill>()}, S::PartiallyFilled},
      {{S::PartiallyFilled, column<CancelRequest>()}, S::PendingCancel},
      {{S::PartiallyFilled, column<ReplaceRequest>()}, S::PendingReplace},
      {{S::PartiallyFilled, column<VenueCancel>()}, S::Canceled},

      // The venue may refuse a request after the order filled: a harmless no-op.
      {{S::Filled, column<CancelReject>()}, S::Filled},
      {{S::Filled, column<ReplaceReject>()}, S::Filled},

      {{S::PendingCancel, column<Fill>()}, S::PendingCancel}, // traded before the cancel landed
      {{S::PendingCancel, column<CancelDone>()}, S::Canceled},
      {{S::PendingCancel, column<CancelReject>()}, S::PartiallyFilled},
      {{S::PendingCancel, column<VenueCancel>()}, S::Canceled},

      {{S::PendingReplace, column<Fill>()}, S::PendingReplace},
      {{S::PendingReplace, column<ReplaceDone>()}, S::PartiallyFilled},
      {{S::PendingReplace, column<ReplaceReject>()}, S::PartiallyFilled},
      {{S::PendingReplace, column<VenueCancel>()}, S::Canceled},
  };
  return moves;
}

// --- Every (state, event) pair --------------------------------------------------

// An event column, printed by name so test names read "(Filled, CancelDone)".
struct EventColumn {
  std::size_t index;
};
void PrintTo(EventColumn column, std::ostream* os) { *os << eventName(column.index); }

std::array<EventColumn, kEventCount> allEventColumns() {
  std::array<EventColumn, kEventCount> columns{};
  for (std::size_t i = 0; i < kEventCount; ++i) columns[i] = EventColumn{i};
  return columns;
}

class TransitionTest : public ::testing::TestWithParam<std::tuple<OrdState, EventColumn>> {};

TEST_P(TransitionTest, MatchesTheLifecycleSpec) {
  const auto [state, column] = GetParam();
  Order order = orderIn(state);
  const Order before = order;
  const ApplyResult result = order.apply(sampleEvent(column.index));

  const auto legal = legalMoves().find({state, column.index});
  if (legal != legalMoves().end()) {
    EXPECT_EQ(result, ApplyResult::Ok);
    EXPECT_EQ(order.state(), legal->second);
  } else {
    EXPECT_EQ(result, ApplyResult::IllegalTransition);
    EXPECT_EQ(order, before); // refused: nothing changed
  }
}

std::string transitionName(const ::testing::TestParamInfo<TransitionTest::ParamType>& info) {
  return std::string(toString(std::get<0>(info.param))) + "_" +
         std::string(eventName(std::get<1>(info.param).index));
}

INSTANTIATE_TEST_SUITE_P(AllPairs, TransitionTest,
                         ::testing::Combine(::testing::ValuesIn(kAllStates),
                                            ::testing::ValuesIn(allEventColumns())),
                         transitionName);

// --- Scenarios ------------------------------------------------------------------

TEST(OrderLifecycle, FillsToCompletionWithAveragePrice) {
  Order order = orderIn(OrdState::New);
  ASSERT_EQ(order.apply(Fill{30, Price::fromRaw(100'000)}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::PartiallyFilled);
  EXPECT_EQ(order.leavesQty(), 70);

  ASSERT_EQ(order.apply(Fill{70, Price::fromRaw(100'200)}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Filled);
  EXPECT_EQ(order.cumQty(), 100);
  EXPECT_EQ(order.leavesQty(), 0);
  EXPECT_EQ(order.avgPx(), Price::fromRaw(100'140)); // (30 × 10.00 + 70 × 10.02) / 100 = 10.014
}

TEST(OrderLifecycle, AveragePriceRoundsToNearestTenThousandth) {
  Order order(1, 0, 0, Side::Buy, kPx, 300);
  ASSERT_EQ(order.apply(Accept{}), ApplyResult::Ok);
  ASSERT_EQ(order.apply(Fill{100, Price::fromRaw(100'000)}), ApplyResult::Ok);
  ASSERT_EQ(order.apply(Fill{200, Price::fromRaw(100'100)}), ApplyResult::Ok);
  EXPECT_EQ(order.avgPx(), Price::fromRaw(100'067)); // exactly 10.006666…
}

TEST(OrderLifecycle, AveragePriceIsZeroBeforeAnyFill) {
  EXPECT_EQ(orderIn(OrdState::New).avgPx(), Price{});
}

TEST(OrderLifecycle, FillThatCompletesOrderBeatsPendingCancel) {
  // The venue traded the rest of the order before it saw our cancel.
  Order order = orderIn(OrdState::PendingCancel); // 40 of 100 filled
  ASSERT_EQ(order.apply(Fill{60, kPx}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Filled);

  // The venue then refuses the cancel as too late: accepted, nothing changes...
  EXPECT_EQ(order.apply(CancelReject{}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Filled);
  // ...and it can't confirm a cancel of a filled order.
  EXPECT_EQ(order.apply(CancelDone{}), ApplyResult::IllegalTransition);
}

TEST(OrderLifecycle, PartialFillWhileCancelPendingIsKept) {
  Order order = orderIn(OrdState::PendingCancel); // 40 of 100 filled
  ASSERT_EQ(order.apply(Fill{10, kPx}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::PendingCancel);

  ASSERT_EQ(order.apply(CancelDone{}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Canceled);
  EXPECT_EQ(order.cumQty(), 50);
  EXPECT_EQ(order.leavesQty(), 0);
}

TEST(OrderLifecycle, RefusedCancelReturnsToTheLiveState) {
  Order untouched = orderIn(OrdState::New);
  ASSERT_EQ(untouched.apply(CancelRequest{}), ApplyResult::Ok);
  ASSERT_EQ(untouched.apply(CancelReject{}), ApplyResult::Ok);
  EXPECT_EQ(untouched.state(), OrdState::New);

  Order partial = orderIn(OrdState::PendingCancel); // 40 of 100 filled
  ASSERT_EQ(partial.apply(CancelReject{}), ApplyResult::Ok);
  EXPECT_EQ(partial.state(), OrdState::PartiallyFilled);
}

TEST(OrderLifecycle, ReplaceChangesPriceAndQuantity) {
  Order order = orderIn(OrdState::PendingReplace); // 40 of 100 filled
  ASSERT_EQ(order.apply(ReplaceDone{150, Price::fromRaw(105'000)}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::PartiallyFilled);
  EXPECT_EQ(order.orderQty(), 150);
  EXPECT_EQ(order.price(), Price::fromRaw(105'000));
  EXPECT_EQ(order.leavesQty(), 110);
}

TEST(OrderLifecycle, ReplaceDownToFilledQuantityCompletesOrder) {
  Order order = orderIn(OrdState::PendingReplace); // 40 of 100 filled
  ASSERT_EQ(order.apply(ReplaceDone{40, kPx}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Filled);
  EXPECT_EQ(order.leavesQty(), 0);
}

TEST(OrderLifecycle, ReplaceBelowFilledQuantityIsRefused) {
  Order order = orderIn(OrdState::PendingReplace); // 40 of 100 filled
  const Order before = order;
  EXPECT_EQ(order.apply(ReplaceDone{30, kPx}), ApplyResult::InvalidQuantity);
  EXPECT_EQ(order.apply(ReplaceDone{0, kPx}), ApplyResult::InvalidQuantity);
  EXPECT_EQ(order, before);
}

TEST(OrderLifecycle, OverfillIsRefused) {
  Order order = orderIn(OrdState::PartiallyFilled); // 40 of 100 filled
  const Order before = order;
  EXPECT_EQ(order.apply(Fill{61, kPx}), ApplyResult::Overfill);
  EXPECT_EQ(order, before);

  EXPECT_EQ(order.apply(Fill{60, kPx}), ApplyResult::Ok); // exactly the rest is fine
  EXPECT_EQ(order.state(), OrdState::Filled);
}

TEST(OrderLifecycle, NonPositiveFillIsRefused) {
  Order order = orderIn(OrdState::New);
  const Order before = order;
  EXPECT_EQ(order.apply(Fill{0, kPx}), ApplyResult::InvalidQuantity);
  EXPECT_EQ(order.apply(Fill{-5, kPx}), ApplyResult::InvalidQuantity);
  EXPECT_EQ(order, before);
}

TEST(OrderLifecycle, VenueCancelsUnfilledRestOfIocOrder) {
  Order order = orderIn(OrdState::New);
  ASSERT_EQ(order.apply(Fill{30, kPx}), ApplyResult::Ok);
  ASSERT_EQ(order.apply(VenueCancel{}), ApplyResult::Ok);
  EXPECT_EQ(order.state(), OrdState::Canceled);
  EXPECT_EQ(order.cumQty(), 30);
  EXPECT_EQ(order.leavesQty(), 0);
}

// --- Random sequences -------------------------------------------------------------

// Throws 250,000 random events (including invalid quantities) at fresh orders and
// checks the rules that must hold after every single one.
TEST(OrderLifecycle, RandomEventSequencesKeepTheInvariants) {
  std::mt19937 rng(20261011); // fixed seed: the same sequences every run
  std::uniform_int_distribution<std::size_t> pickEvent(0, kEventCount - 1);
  std::uniform_int_distribution<Qty> fillQty(-5, 60);
  std::uniform_int_distribution<Qty> replaceQty(-5, 150);
  std::uniform_int_distribution<std::int64_t> priceRaw(90'000, 110'000);
  std::array<int, kOrdStateCount> timesSeen{};

  for (int sequence = 0; sequence < 10'000; ++sequence) {
    Order order(1, 0, 0, Side::Buy, kPx, 100);
    for (int step = 0; step < 25; ++step) {
      OrderEvent e = sampleEvent(pickEvent(rng));
      if (auto* fill = std::get_if<Fill>(&e)) *fill = Fill{fillQty(rng), Price::fromRaw(priceRaw(rng))};
      if (auto* r = std::get_if<ReplaceDone>(&e)) *r = ReplaceDone{replaceQty(rng), Price::fromRaw(priceRaw(rng))};

      const Order before = order;
      const ApplyResult result = order.apply(e);

      if (result != ApplyResult::Ok) {
        ASSERT_EQ(order, before); // refused: unchanged
      }
      if (isTerminal(before.state())) {
        ASSERT_EQ(order.state(), before.state()); // done stays done
      }
      if (order.state() == OrdState::Filled) {
        ASSERT_EQ(order.cumQty(), order.orderQty());
      }
      ASSERT_GE(order.cumQty(), before.cumQty()); // fills are never undone
      ASSERT_GE(order.cumQty(), 0);
      ASSERT_LE(order.cumQty(), order.orderQty()); // never overfilled
      ASSERT_EQ(order.leavesQty() == 0, isTerminal(order.state())); // open exactly while live
      ++timesSeen[static_cast<std::size_t>(order.state())];
    }
  }
  for (const OrdState state : kAllStates) {
    EXPECT_GT(timesSeen[static_cast<std::size_t>(state)], 0) << toString(state) << " never reached";
  }
}

} // namespace
} // namespace oems
