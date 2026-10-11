// Where the time goes for one order inside the gateway, one stage at a time:
//
//   1. QuickFIX parses the raw message into a FIX::Message
//   2. QuickFIX validates it against the FIX 4.4 data dictionary
//   3. the gateway turns it into a ClientNewOrder (field lookups, exact decimal parsing)
//   4. the order manager handles it: duplicate check, risk, matching, state
//      machine, exposure, reports as plain structs
//   5. the gateway turns each report into a FIX ExecutionReport and serialises it
//
// What isn't here, and shows up only end to end (oems_load_client): the session
// layer (sequence numbers, storing each sent message), sockets, and the client's
// own work on the same machine.

#include "oems/config.hpp"
#include "oems/fix_gateway.hpp"
#include "oems/fix_settings.hpp"
#include "oems/in_process_venue.hpp"
#include "oems/order_manager.hpp"
#include "oems/risk.hpp"

#include <benchmark/benchmark.h>

#include <quickfix/DataDictionary.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixFields.h>
#include <quickfix/Message.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <sstream>
#include <string>

namespace {

using namespace oems;

// A typical NewOrderSingle as it arrives on the wire, header included.
std::string wireNewOrder() {
  FIX44::NewOrderSingle m;
  m.getHeader().setField(FIX::FIELD::SenderCompID, "CLIENT1");
  m.getHeader().setField(FIX::FIELD::TargetCompID, "MINIOEMS");
  m.getHeader().setField(FIX::FIELD::MsgSeqNum, "12345");
  m.getHeader().setField(FIX::SendingTime());
  m.setField(FIX::FIELD::ClOrdID, "L123456");
  m.setField(FIX::FIELD::Account, "ACC1");
  m.setField(FIX::FIELD::Symbol, "AAPL");
  m.setField(FIX::FIELD::Side, "1");
  m.setField(FIX::TransactTime());
  m.setField(FIX::FIELD::OrdType, "2");
  m.setField(FIX::FIELD::Price, "150.25");
  m.setField(FIX::FIELD::OrderQty, "100");
  m.setField(FIX::FIELD::TimeInForce, "0");
  return m.toString();
}

const FIX::DataDictionary& dictionary() {
  static const FIX::DataDictionary dd(fix44DictionaryPath());
  return dd;
}

struct Directories {
  Directories() {
    symbols.add("AAPL");
    accounts.add("ACC1");
  }
  Directory symbols;
  Directory accounts;
};

void BM_1_QuickFixParse(benchmark::State& state) {
  const std::string wire = wireNewOrder();
  for (auto _ : state) {
    FIX::Message m(wire, dictionary(), /*validate=*/false);
    benchmark::DoNotOptimize(m);
  }
}
BENCHMARK(BM_1_QuickFixParse);

void BM_2_QuickFixValidate(benchmark::State& state) {
  const FIX::Message m(wireNewOrder(), dictionary(), false);
  for (auto _ : state) dictionary().validate(m);
}
BENCHMARK(BM_2_QuickFixValidate);

void BM_3_GatewayTranslate(benchmark::State& state) {
  const Directories dirs;
  const FIX::Message m(wireNewOrder(), dictionary(), false);
  for (auto _ : state) {
    ClientNewOrder order = toClientNewOrder(m, 0, dirs.symbols, dirs.accounts);
    benchmark::DoNotOptimize(order);
  }
}
BENCHMARK(BM_3_GatewayTranslate);

class CountingSink : public ReportSink {
public:
  void onExecutionReport(const ExecutionReport&) override { ++reports; }
  void onCancelReject(const CancelReject&) override { ++reports; }
  std::size_t reports = 0;
};

// Alternating buys and sells at one price, so every second order trades and the
// book stays empty. That's three reports per pair. Fixed iterations, because
// every order stays in the order manager's records.
void BM_4_OrderManager(benchmark::State& state) {
  RiskEngine risk({SymbolLimits{true, 10'000, Price::fromRaw(100), Price::fromUnits(150), 500}},
                  {AccountLimits{notional(10'000'000), 1'000'000, notional(1'000'000'000'000)}});
  OrderManager manager(risk);
  InProcessVenue venue(manager);
  CountingSink sink;
  manager.connect(venue, sink);

  ClientNewOrder order;
  order.price = Price::fromUnits(150);
  order.qty = 100;
  std::uint64_t n = 0;
  for (auto _ : state) {
    order.clOrdId = "L" + std::to_string(n);
    order.side = n % 2 == 0 ? Side::Buy : Side::Sell;
    manager.onNewOrder(order);
    ++n;
  }
  benchmark::DoNotOptimize(sink.reports);
  state.counters["reports_per_order"] = static_cast<double>(sink.reports) / static_cast<double>(n);
}
BENCHMARK(BM_4_OrderManager)->Iterations(500'000);

void BM_5_ExecutionReportToWire(benchmark::State& state) {
  const Directories dirs;
  ExecutionReport r;
  r.orderId = 123456;
  r.execId = 987654;
  r.execType = ExecType::Trade;
  r.state = OrdState::Filled;
  r.clOrdId = "L123456";
  r.price = Price::fromRaw(1'502'500);
  r.orderQty = 100;
  r.lastQty = 100;
  r.lastPx = Price::fromRaw(1'502'500);
  r.cumQty = 100;
  r.avgPx = Price::fromRaw(1'502'500);
  for (auto _ : state) {
    FIX::Message m = toFix(r, dirs.symbols, dirs.accounts);
    std::string wire = m.toString();
    benchmark::DoNotOptimize(wire);
  }
}
BENCHMARK(BM_5_ExecutionReportToWire);

} // namespace

BENCHMARK_MAIN();
