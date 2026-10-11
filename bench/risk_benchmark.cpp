// How long one pre-trade risk check takes.
//
// Setup: 500 symbols and 50 accounts, and 4,096 different pre-built orders
// spread over them (random symbol, account, side, price within the band, and
// quantity), cycled through in order. That way the benchmark doesn't measure one
// cached case over and over, and making the orders isn't part of the timing.

#include "oems/risk.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using namespace oems;

constexpr std::size_t kSymbols = 500;
constexpr std::size_t kAccounts = 50;
constexpr std::size_t kOrders = 4096; // a power of two, so cycling is a cheap mask

RiskEngine makeEngine() {
  // Tick 0.01, reference 100.00, band ±10%; generous account limits.
  const std::vector<SymbolLimits> symbols(
      kSymbols, SymbolLimits{true, 100'000, Price::fromRaw(100), Price::fromUnits(100), 1'000});
  const std::vector<AccountLimits> accounts(
      kAccounts, AccountLimits{notional(100'000'000), 10'000'000, notional(1'000'000'000'000)});
  return RiskEngine(symbols, accounts);
}

// Orders that pass every check.
std::vector<RiskOrder> makeOrders() {
  std::mt19937 rng(42);
  std::uniform_int_distribution<SymbolId> symbol(0, kSymbols - 1);
  std::uniform_int_distribution<AccountId> account(0, kAccounts - 1);
  std::uniform_int_distribution<std::int64_t> cents(9'000, 11'000);
  std::uniform_int_distribution<Qty> qty(1, 1'000);
  std::vector<RiskOrder> orders;
  for (std::size_t i = 0; i < kOrders; ++i) {
    orders.push_back({account(rng), symbol(rng), i % 2 == 0 ? Side::Buy : Side::Sell,
                      Price::fromRaw(cents(rng) * 100), qty(rng)});
  }
  return orders;
}

// The full check on an order that passes: every check runs.
void BM_CheckNewPassingOrder(benchmark::State& state) {
  const RiskEngine risk = makeEngine();
  const std::vector<RiskOrder> orders = makeOrders();
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(risk.checkNew(orders[i]));
    i = (i + 1) & (kOrders - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CheckNewPassingOrder);

// Rejected by the first check: the cheapest possible path.
void BM_CheckNewUnknownAccount(benchmark::State& state) {
  const RiskEngine risk = makeEngine();
  std::vector<RiskOrder> orders = makeOrders();
  for (RiskOrder& order : orders) order.account = kAccounts; // one past the last account
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(risk.checkNew(orders[i]));
    i = (i + 1) & (kOrders - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CheckNewUnknownAccount);

// What risk does for an order that passes, is accepted and fills completely:
// the check, plus the two exposure updates.
void BM_CheckAcceptAndFill(benchmark::State& state) {
  RiskEngine risk = makeEngine();
  const std::vector<RiskOrder> orders = makeOrders();
  std::size_t i = 0;
  for (auto _ : state) {
    const RiskOrder& order = orders[i];
    if (risk.checkNew(order) == RiskReject::None) {
      risk.onOpenChange(order.account, order.symbol, order.side, order.qty);
      risk.onFill(order.account, order.symbol, order.side, order.qty);
    }
    i = (i + 1) & (kOrders - 1);
  }
  benchmark::DoNotOptimize(risk.grossExposure(0));
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CheckAcceptAndFill);

} // namespace

BENCHMARK_MAIN();
