// oems_load_client: sends synthetic orders to a running mini_oems over FIX and
// measures throughput and acknowledgement latency.
//
//   oems_load_client <session config> <instruments.csv> <account> <orders> <max in flight>
//
// Flow: for each tradable symbol in turn, a buy and then a sell of 100 at the
// symbol's reference price, so every sell trades with the buy before it and the
// book stays small. Half the orders trade, which makes three ExecutionReports per
// buy/sell pair. An order counts as acknowledged at its first ExecutionReport
// (New or Rejected).
//
// "max in flight" caps how many orders may be sent but not yet acknowledged.
// Large (say 1000) measures throughput; 1 measures unloaded round-trip latency.
// All orders are built before timing starts, so building them isn't measured.

#include "oems/config.hpp"
#include "oems/fix_settings.hpp"

#include <quickfix/Application.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixFields.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

class LoadClient : public FIX::Application {
public:
  explicit LoadClient(std::size_t orders) : ackNs_(orders, 0) {}

  void onCreate(const FIX::SessionID&) override {}
  void onLogon(const FIX::SessionID&) override {
    {
      std::lock_guard lock(mutex_);
      loggedOn_ = true;
    }
    changed_.notify_all();
  }
  void onLogout(const FIX::SessionID&) override {}
  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void toApp(FIX::Message&, const FIX::SessionID&) override {}
  void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}

  void fromApp(const FIX::Message& m, const FIX::SessionID&) override {
    if (m.getHeader().getField(FIX::FIELD::MsgType) != "8") return;
    const std::int64_t now = nowNs();
    const std::string& execType = m.getField(FIX::FIELD::ExecType);
    const std::string& clOrdId = m.getField(FIX::FIELD::ClOrdID); // "L<number>-<run>"
    if (clOrdId.size() < 2 || clOrdId[0] != 'L') return;           // not one of ours
    const std::size_t n = std::stoull(clOrdId.substr(1));          // stops at the '-'
    bool acknowledged = false;
    {
      std::lock_guard lock(mutex_);
      if (execType == "F") ++trades_;
      if ((execType == "0" || execType == "8") && n < ackNs_.size() && ackNs_[n] == 0) {
        ackNs_[n] = now;
        ++acked_;
        if (execType == "8") ++rejected_;
        acknowledged = true;
      }
    }
    if (acknowledged) changed_.notify_all();
  }

  bool waitForLogon(std::chrono::seconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return loggedOn_; });
  }
  // Waits until at most `limit` sent orders are unacknowledged.
  bool waitForInFlightAtMost(std::size_t sent, std::size_t limit, std::chrono::seconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return sent - acked_ <= limit; });
  }

  std::vector<std::int64_t> ackTimes() {
    std::lock_guard lock(mutex_);
    return ackNs_;
  }
  std::size_t trades() {
    std::lock_guard lock(mutex_);
    return trades_;
  }
  std::size_t rejected() {
    std::lock_guard lock(mutex_);
    return rejected_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool loggedOn_ = false;
  std::vector<std::int64_t> ackNs_; // indexed by order number
  std::size_t acked_ = 0;
  std::size_t trades_ = 0;
  std::size_t rejected_ = 0;
};

double percentile(const std::vector<std::int64_t>& sorted, double p) {
  const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
  return static_cast<double>(sorted[index]) / 1'000.0; // ns -> µs
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: oems_load_client <session config> <instruments.csv> <account> <orders> <max in flight>\n";
    return 2;
  }
  try {
    const std::string account = argv[3];
    const std::size_t orderCount = std::stoull(argv[4]);
    const std::size_t maxInFlight = std::max<std::size_t>(1, std::stoull(argv[5]));

    std::ifstream instrumentsFile(argv[2]);
    if (!instrumentsFile) throw std::runtime_error(std::string("can't open ") + argv[2]);
    const oems::InstrumentConfig instruments = oems::loadInstruments(instrumentsFile);
    std::vector<std::pair<std::string, std::string>> symbols; // name, reference price
    for (std::uint32_t id = 0; id < instruments.symbols.size(); ++id) {
      if (instruments.limits[id].tradable) {
        symbols.emplace_back(instruments.symbols.name(id), oems::formatPrice(instruments.limits[id].referencePrice));
      }
    }
    if (symbols.empty()) throw std::runtime_error("no tradable symbols");

    // Build every order up front. ClOrdIDs are "L<number>-<run>": the gateway
    // remembers every ClOrdID, so each run needs its own.
    const std::string run = std::to_string(nowNs());
    std::vector<FIX44::NewOrderSingle> orders(orderCount);
    for (std::size_t i = 0; i < orderCount; ++i) {
      const auto& [symbol, price] = symbols[(i / 2) % symbols.size()];
      FIX44::NewOrderSingle& m = orders[i];
      m.setField(FIX::FIELD::ClOrdID, "L" + std::to_string(i) + "-" + run);
      m.setField(FIX::FIELD::Account, account);
      m.setField(FIX::FIELD::Symbol, symbol);
      m.setField(FIX::FIELD::Side, i % 2 == 0 ? "1" : "2");
      m.setField(FIX::TransactTime());
      m.setField(FIX::FIELD::OrdType, "2");
      m.setField(FIX::FIELD::Price, price);
      m.setField(FIX::FIELD::OrderQty, "100");
      m.setField(FIX::FIELD::TimeInForce, "0");
    }

    const FIX::SessionSettings settings = oems::loadSessionSettings(argv[1]);
    const FIX::SessionID session = *settings.getSessions().begin();
    LoadClient client(orderCount);
    FIX::MemoryStoreFactory store;
    FIX::SocketInitiator initiator(client, store, settings);
    initiator.start();
    if (!client.waitForLogon(std::chrono::seconds(15))) throw std::runtime_error("logon timed out");

    std::vector<std::int64_t> sendNs(orderCount);
    const std::int64_t start = nowNs();
    for (std::size_t i = 0; i < orderCount; ++i) {
      if (!client.waitForInFlightAtMost(i, maxInFlight - 1, std::chrono::seconds(30))) {
        throw std::runtime_error("no acknowledgements for 30 s after order " + std::to_string(i));
      }
      sendNs[i] = nowNs();
      FIX::Session::sendToTarget(orders[i], session);
    }
    if (!client.waitForInFlightAtMost(orderCount, 0, std::chrono::seconds(60))) {
      throw std::runtime_error("not every order was acknowledged");
    }

    const std::vector<std::int64_t> ackNs = client.ackTimes();
    const std::int64_t end = *std::max_element(ackNs.begin(), ackNs.end());
    std::vector<std::int64_t> latency(orderCount);
    for (std::size_t i = 0; i < orderCount; ++i) latency[i] = ackNs[i] - sendNs[i];
    std::sort(latency.begin(), latency.end());
    const double seconds = static_cast<double>(end - start) / 1e9;

    std::printf("orders sent:       %zu (max %zu in flight, account %s, %zu symbols)\n", orderCount, maxInFlight,
                account.c_str(), symbols.size());
    std::printf("acknowledged:      %zu (%zu rejected)\n", orderCount, client.rejected());
    std::printf("trade reports:     %zu\n", client.trades());
    std::printf("elapsed:           %.3f s\n", seconds);
    std::printf("throughput:        %.0f orders/s\n", static_cast<double>(orderCount) / seconds);
    std::printf("ack latency (us):  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", percentile(latency, 0.50),
                percentile(latency, 0.90), percentile(latency, 0.99), percentile(latency, 0.999),
                percentile(latency, 1.0));

    initiator.stop();
    return client.rejected() == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "oems_load_client: " << e.what() << '\n';
    return 1;
  }
}
