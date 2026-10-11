#include "oems/risk.hpp"
#include "printers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace oems {

namespace {

constexpr Price px(std::int64_t cents) { return Price::fromRaw(cents * 100); } // px(10'000) is 100.00

// Symbols. Each has 1-cent ticks and a 5% price band.
constexpr SymbolId kLiquid = 0; // around 100.00
constexpr SymbolId kHalted = 1;
constexpr SymbolId kCheap = 2;  // around 10.00
constexpr SymbolId kHuge = 3;   // around 1,000,000,000.00, with no quantity cap: for overflow

// Accounts, each with one limit that's easy to reach.
constexpr AccountId kNormal = 0;      // nothing here is close to its limits
constexpr AccountId kSmallOrders = 1; // max 50,000 notional per order
constexpr AccountId kPositions = 2;   // max 1,000 worst-case position per symbol
constexpr AccountId kGross = 3;       // max 150,000 gross exposure

RiskEngine makeEngine() {
  const SymbolLimits liquid{true, 1'000, px(1), px(10'000), 500};
  const SymbolLimits halted{false, 1'000, px(1), px(2'000), 500};
  const SymbolLimits cheap{true, 10'000, px(1), px(1'000), 500};
  const SymbolLimits huge{true, 1'000'000'000, px(1), px(100'000'000'000), 500};
  return RiskEngine({liquid, halted, cheap, huge},
                    {AccountLimits{notional(500'000), 100'000, notional(1'000'000'000)},
                     AccountLimits{notional(50'000), 100'000, notional(1'000'000'000)},
                     AccountLimits{notional(1'000'000'000), 1'000, notional(1'000'000'000)},
                     AccountLimits{notional(1'000'000'000), 100'000, notional(150'000)}});
}

RiskOrder buy(AccountId account, SymbolId symbol, std::int64_t cents, Qty qty) {
  return {account, symbol, Side::Buy, px(cents), qty};
}
RiskOrder sell(AccountId account, SymbolId symbol, std::int64_t cents, Qty qty) {
  return {account, symbol, Side::Sell, px(cents), qty};
}

// Puts exposure in place directly, as accepted orders and fills would.
void hold(RiskEngine& risk, AccountId a, SymbolId s, Qty position, Qty openBuy, Qty openSell) {
  if (position > 0) {
    risk.onOpenChange(a, s, Side::Buy, position);
    risk.onFill(a, s, Side::Buy, position);
  } else if (position < 0) {
    risk.onOpenChange(a, s, Side::Sell, -position);
    risk.onFill(a, s, Side::Sell, -position);
  }
  if (openBuy > 0) risk.onOpenChange(a, s, Side::Buy, openBuy);
  if (openSell > 0) risk.onOpenChange(a, s, Side::Sell, openSell);
}

// --- Checks on the order itself --------------------------------------------------

TEST(RiskChecks, AcceptsOrderWithinEveryLimit) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'000, 100)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(sell(kNormal, kLiquid, 10'000, 100)), RiskReject::None);
}

TEST(RiskChecks, UnknownAccountOrSymbol) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(99, kLiquid, 10'000, 100)), RiskReject::UnknownAccount);
  EXPECT_EQ(risk.checkNew(buy(kNormal, 99, 10'000, 100)), RiskReject::UnknownSymbol);
}

TEST(RiskChecks, HaltedSymbol) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, kHalted, 2'000, 100)), RiskReject::SymbolNotTradable);
}

TEST(RiskChecks, QuantityMustBePositiveAndWithinMax) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'000, 0)), RiskReject::InvalidQuantity);
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'000, -5)), RiskReject::InvalidQuantity);
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'000, 1'000)), RiskReject::None); // exactly the max
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'000, 1'001)), RiskReject::QuantityAboveMax);
}

TEST(RiskChecks, PriceMustBePositiveAndOnTick) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 0, 100)), RiskReject::InvalidPrice);
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, -100, 100)), RiskReject::InvalidPrice);
  const RiskOrder halfCent{kNormal, kLiquid, Side::Buy, Price::fromRaw(1'000'050), 100}; // 100.005
  EXPECT_EQ(risk.checkNew(halfCent), RiskReject::OffTick);
}

TEST(RiskChecks, PriceBandIsInclusive) {
  const RiskEngine risk = makeEngine(); // 5% around 100.00
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'500, 10)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(buy(kNormal, kLiquid, 10'501, 10)), RiskReject::OutsidePriceBand);
  EXPECT_EQ(risk.checkNew(sell(kNormal, kLiquid, 9'500, 10)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(sell(kNormal, kLiquid, 9'499, 10)), RiskReject::OutsidePriceBand);
}

TEST(RiskChecks, FatFingerQuantityIsCaught) {
  // Mizuho 2005, scaled down: meant to sell 1 at 610,000; typed 610,000 at 1.
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(sell(kNormal, kLiquid, 100, 610'000)), RiskReject::QuantityAboveMax);
}

TEST(RiskChecks, NotionalIsInclusive) {
  const RiskEngine risk = makeEngine(); // 50,000 per order
  EXPECT_EQ(risk.checkNew(buy(kSmallOrders, kLiquid, 10'000, 500)), RiskReject::None); // 500 × 100.00
  EXPECT_EQ(risk.checkNew(buy(kSmallOrders, kLiquid, 10'000, 501)), RiskReject::NotionalAboveMax);
}

TEST(RiskChecks, HugeNotionalDoesNotOverflow) {
  // 1,000,000,000 × 1e9 is far past the 64-bit range. Computed in 64 bits it
  // would wrap, possibly to a small number, and pass.
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, kHuge, 100'000'000'000, 1'000'000'000)), RiskReject::NotionalAboveMax);
}

TEST(RiskChecks, FirstFailingCheckDecides) {
  const RiskEngine risk = makeEngine();
  EXPECT_EQ(risk.checkNew(buy(kNormal, 99, 0, 0)), RiskReject::UnknownSymbol);
  EXPECT_EQ(risk.checkNew(buy(kNormal, kHalted, 0, 0)), RiskReject::SymbolNotTradable);
  const RiskOrder offTickAndOutsideBand{kNormal, kLiquid, Side::Buy, Price::fromRaw(2'000'050), 10};
  EXPECT_EQ(risk.checkNew(offTickAndOutsideBand), RiskReject::OffTick);
}

TEST(RiskChecks, ZeroTickSizeIsRefused) {
  EXPECT_THROW(RiskEngine({SymbolLimits{true, 10, Price{}, px(100), 500}}, {}), std::invalid_argument);
}

// --- Checks on exposure ------------------------------------------------------------

TEST(RiskExposure, WorstCaseCountsOpenOrders) {
  // The plan's interview question: limit 1,000, long 900, open buy 200. A new buy
  // of 50 could take the position to 1,150, so it's refused. A sell can only
  // reduce the worst case, so it's allowed even though the account is already
  // past its limit.
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, 900, 200, 0);
  EXPECT_EQ(RiskEngine::worstCase(risk.exposure(kPositions, kLiquid)), 1'100);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 50)), RiskReject::PositionLimit);
  EXPECT_EQ(risk.checkNew(sell(kPositions, kLiquid, 10'000, 50)), RiskReject::None);
}

TEST(RiskExposure, PositionLimitIsInclusive) {
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, 600, 0, 0);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 400)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 401)), RiskReject::PositionLimit);
}

TEST(RiskExposure, ShortSideIsLimitedToo) {
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, -900, 0, 0);
  EXPECT_EQ(risk.checkNew(sell(kPositions, kLiquid, 10'000, 101)), RiskReject::PositionLimit);
  EXPECT_EQ(risk.checkNew(sell(kPositions, kLiquid, 10'000, 100)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 500)), RiskReject::None); // reduces the short
}

TEST(RiskExposure, OpenOrdersOnBothSidesCountSeparately) {
  // Long and short open orders don't cancel out: either side could fill alone.
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, 0, 600, 600);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 400)), RiskReject::None);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kLiquid, 10'000, 401)), RiskReject::PositionLimit);
}

TEST(RiskExposure, LimitIsPerSymbol) {
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, 1'000, 0, 0);
  EXPECT_EQ(risk.checkNew(buy(kPositions, kCheap, 1'000, 1'000)), RiskReject::None);
}

TEST(RiskExposure, GrossExposureAddsUpAcrossSymbols) {
  // Limit 150,000. Valued at reference prices: 1,000 × 100.00 + 4,000 × 10.00 = 140,000.
  RiskEngine risk = makeEngine();
  hold(risk, kGross, kLiquid, 1'000, 0, 0);
  hold(risk, kGross, kCheap, 4'000, 0, 0);
  EXPECT_EQ(risk.grossExposure(kGross), notional(140'000));
  EXPECT_EQ(risk.checkNew(buy(kGross, kCheap, 1'000, 1'000)), RiskReject::None); // exactly 150,000
  EXPECT_EQ(risk.checkNew(buy(kGross, kCheap, 1'000, 1'001)), RiskReject::GrossExposureLimit);
}

TEST(RiskExposure, BookkeepingFollowsTheOrderLifecycle) {
  RiskEngine risk = makeEngine();
  risk.onOpenChange(kNormal, kLiquid, Side::Buy, 300); // buy 300 accepted
  EXPECT_EQ(risk.grossExposure(kNormal), notional(30'000));

  risk.onFill(kNormal, kLiquid, Side::Buy, 100); // 100 fill: worst case unchanged
  EXPECT_EQ(risk.exposure(kNormal, kLiquid).position, 100);
  EXPECT_EQ(risk.exposure(kNormal, kLiquid).openBuy, 200);
  EXPECT_EQ(risk.grossExposure(kNormal), notional(30'000));

  risk.onOpenChange(kNormal, kLiquid, Side::Buy, -200); // rest cancelled
  EXPECT_EQ(risk.grossExposure(kNormal), notional(10'000));

  risk.onOpenChange(kNormal, kLiquid, Side::Sell, 100); // sell 100 accepted and filled: flat
  risk.onFill(kNormal, kLiquid, Side::Sell, 100);
  EXPECT_EQ(risk.exposure(kNormal, kLiquid).position, 0);
  EXPECT_EQ(risk.grossExposure(kNormal), 0);
}

// --- Replace -------------------------------------------------------------------------

TEST(RiskReplace, ReducingAnOrderIsAllowedEvenOverLimit) {
  RiskEngine risk = makeEngine();
  hold(risk, kPositions, kLiquid, 900, 200, 0); // worst case 1,100: already past 1,000
  // Down to 150: the worst case falls to 1,050, still over the limit, but lower.
  const RiskReplace smaller{kPositions, kLiquid, Side::Buy, 200, 0, px(10'000), 150};
  EXPECT_EQ(risk.checkReplace(smaller), RiskReject::None);
  const RiskReplace larger{kPositions, kLiquid, Side::Buy, 200, 0, px(10'000), 300};
  EXPECT_EQ(risk.checkReplace(larger), RiskReject::PositionLimit);
}

TEST(RiskReplace, NewPriceAndQuantityAreChecked) {
  RiskEngine risk = makeEngine();
  hold(risk, kNormal, kLiquid, 0, 100, 0);
  const RiskReplace outsideBand{kNormal, kLiquid, Side::Buy, 100, 0, px(10'600), 100};
  EXPECT_EQ(risk.checkReplace(outsideBand), RiskReject::OutsidePriceBand);
  const RiskReplace tooBig{kNormal, kLiquid, Side::Buy, 100, 0, px(10'000), 1'001};
  EXPECT_EQ(risk.checkReplace(tooBig), RiskReject::QuantityAboveMax);
}

TEST(RiskReplace, CannotGoBelowWhatHasFilled) {
  const RiskEngine risk = makeEngine();
  const RiskReplace belowFilled{kNormal, kLiquid, Side::Buy, 50, 150, px(10'000), 100};
  EXPECT_EQ(risk.checkReplace(belowFilled), RiskReject::InvalidQuantity);
}

// --- Against a model that recomputes everything --------------------------------------

// 100,000 random checks, acceptances, fills and cancels. The engine keeps gross
// exposure up to date incrementally; the model re-adds every symbol from scratch
// each time. Exposures, gross totals and every check decision must agree.
TEST(RiskModel, IncrementalExposureMatchesFullRecount) {
  constexpr std::size_t kAccounts = 3;
  constexpr std::size_t kSymbols = 4;
  const std::array<std::int64_t, kSymbols> refCents = {10'000, 1'000, 2'500, 50};
  std::vector<SymbolLimits> symbols;
  for (const std::int64_t cents : refCents) symbols.push_back({true, 500, px(1), px(cents), 1'000});
  const std::vector<AccountLimits> accounts(kAccounts, AccountLimits{notional(40'000), 1'500, notional(150'000)});
  RiskEngine risk(symbols, accounts);

  struct Held {
    Qty position = 0, openBuy = 0, openSell = 0;
  };
  std::array<std::array<Held, kSymbols>, kAccounts> model{};
  const auto worst = [](const Held& h) {
    return std::max(std::abs(h.position + h.openBuy), std::abs(h.position - h.openSell));
  };
  const auto grossOf = [&](std::size_t a) {
    Int128 total = 0;
    for (std::size_t s = 0; s < kSymbols; ++s) total += Int128{worst(model[a][s])} * px(refCents[s]).raw();
    return total;
  };

  std::mt19937 rng(20261011);
  std::uniform_int_distribution<std::size_t> pickAccount(0, kAccounts - 1);
  std::uniform_int_distribution<std::size_t> pickSymbol(0, kSymbols - 1);
  std::uniform_int_distribution<int> op(0, 9);
  std::uniform_int_distribution<Qty> qty(1, 400);
  std::uniform_int_distribution<int> bandPercent(-12, 12); // the band is 10%, so some are outside

  for (int step = 0; step < 100'000; ++step) {
    const auto a = static_cast<AccountId>(pickAccount(rng));
    const auto s = static_cast<SymbolId>(pickSymbol(rng));
    const Side side = op(rng) < 5 ? Side::Buy : Side::Sell;
    Held& h = model[a][s];
    Qty& open = side == Side::Buy ? h.openBuy : h.openSell;
    const int choice = op(rng);

    if (choice < 6) { // a new order: check it, and usually accept it if it passes
      const std::int64_t cents = refCents[s] + refCents[s] * bandPercent(rng) / 100;
      const RiskOrder order{a, s, side, px(cents), qty(rng)};

      // The rules, restated from scratch.
      Held after = h;
      (side == Side::Buy ? after.openBuy : after.openSell) += order.qty;
      const std::int64_t distance = std::abs(order.price.raw() - px(refCents[s]).raw());
      RiskReject expected = RiskReject::None;
      if (order.qty > 500) expected = RiskReject::QuantityAboveMax;
      else if (Int128{distance} * 10'000 > Int128{1'000} * px(refCents[s]).raw()) expected = RiskReject::OutsidePriceBand;
      else if (Int128{order.price.raw()} * order.qty > notional(40'000)) expected = RiskReject::NotionalAboveMax;
      else if (worst(after) > worst(h) && worst(after) > 1'500) expected = RiskReject::PositionLimit;
      else if (worst(after) > worst(h) &&
               grossOf(a) + Int128{worst(after) - worst(h)} * px(refCents[s]).raw() > notional(150'000)) {
        expected = RiskReject::GrossExposureLimit;
      }
      ASSERT_EQ(risk.checkNew(order), expected) << "step " << step;

      if (expected == RiskReject::None && choice < 5) {
        risk.onOpenChange(a, s, side, order.qty);
        open += order.qty;
      }
    } else if (choice < 8 && open > 0) { // a fill
      const Qty filled = std::uniform_int_distribution<Qty>(1, open)(rng);
      risk.onFill(a, s, side, filled);
      open -= filled;
      h.position += side == Side::Buy ? filled : -filled;
    } else if (open > 0) { // a cancel of some open quantity
      const Qty cancelled = std::uniform_int_distribution<Qty>(1, open)(rng);
      risk.onOpenChange(a, s, side, -cancelled);
      open -= cancelled;
    }

    const RiskEngine::Exposure& e = risk.exposure(a, s);
    ASSERT_EQ(e.position, h.position) << "step " << step;
    ASSERT_EQ(e.openBuy, h.openBuy) << "step " << step;
    ASSERT_EQ(e.openSell, h.openSell) << "step " << step;
    ASSERT_TRUE(risk.grossExposure(a) == grossOf(a)) << "step " << step;
  }
}

} // namespace
} // namespace oems
