#include "oems/in_process_venue.hpp"
#include "oems/order_manager.hpp"
#include "printers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <ostream>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oems {

namespace {

constexpr Price px(std::int64_t cents) { return Price::fromRaw(cents * 100); } // px(10'000) is 100.00

constexpr SymbolId kStock = 0;  // around 100.00, 1-cent ticks, 5% band, max 1,000 per order
constexpr SymbolId kHalted = 1;
constexpr AccountId kAccount = 0;
constexpr AccountId kTight = 1; // worst-case position limit 500

class Recorder : public ReportSink {
public:
  void onExecutionReport(const ExecutionReport& r) override { reports.push_back(r); }
  void onCancelReject(const CancelReject& r) override { rejects.push_back(r); }
  void clear() {
    reports.clear();
    rejects.clear();
  }
  std::vector<ExecutionReport> reports;
  std::vector<CancelReject> rejects;
};

class OrderManagerTest : public ::testing::Test {
protected:
  OrderManagerTest()
      : risk({SymbolLimits{true, 1'000, px(1), px(10'000), 500}, SymbolLimits{false, 1'000, px(1), px(10'000), 500}},
             {AccountLimits{notional(1'000'000), 100'000, notional(100'000'000)},
              AccountLimits{notional(1'000'000), 500, notional(100'000'000)}}),
        manager(risk),
        venue(manager) {
    manager.connect(venue, sink);
  }

  ClientNewOrder order(ClientId client, std::string id, Side side, std::int64_t cents, Qty qty,
                       TimeInForce tif = TimeInForce::Day, AccountId account = kAccount) {
    ClientNewOrder o;
    o.client = client;
    o.clOrdId = std::move(id);
    o.account = account;
    o.symbol = kStock;
    o.side = side;
    o.price = px(cents);
    o.qty = qty;
    o.tif = tif;
    return o;
  }
  void send(const ClientNewOrder& o) { manager.onNewOrder(o); }
  void cancel(ClientId client, std::string id, std::string orig) {
    manager.onCancel({client, std::move(id), std::move(orig)});
  }
  void replace(ClientId client, std::string id, std::string orig, std::int64_t cents, Qty qty,
               Side side = Side::Buy, SymbolId symbol = kStock) {
    ClientReplace r;
    r.client = client;
    r.clOrdId = std::move(id);
    r.origClOrdId = std::move(orig);
    r.symbol = symbol;
    r.side = side;
    r.price = px(cents);
    r.qty = qty;
    manager.onReplace(r);
  }
  const ExecutionReport& last() const { return sink.reports.back(); }

  RiskEngine risk;
  OrderManager manager;
  InProcessVenue venue;
  Recorder sink;
};

// --- New orders ----------------------------------------------------------------------

TEST_F(OrderManagerTest, AcceptedOrderGetsNewReport) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  ASSERT_EQ(sink.reports.size(), 1U);
  const ExecutionReport& r = last();
  EXPECT_EQ(r.execType, ExecType::New);
  EXPECT_EQ(r.state, OrdState::New);
  EXPECT_EQ(r.clOrdId, "A");
  EXPECT_NE(r.orderId, 0U);
  EXPECT_EQ(r.leavesQty, 100);
  EXPECT_EQ(r.cumQty, 0);
  EXPECT_EQ(r.price, px(10'000));
}

TEST_F(OrderManagerTest, TradeIsReportedToBothClients) {
  send(order(1, "S1", Side::Sell, 10'000, 100));
  send(order(2, "B1", Side::Buy, 10'005, 60)); // pays up to 100.05, trades at 100.00
  ASSERT_EQ(sink.reports.size(), 4U);
  const ExecutionReport& buyer = sink.reports[2];
  const ExecutionReport& seller = sink.reports[3];

  EXPECT_EQ(buyer.client, 2U);
  EXPECT_EQ(buyer.execType, ExecType::Trade);
  EXPECT_EQ(buyer.state, OrdState::Filled);
  EXPECT_EQ(buyer.lastQty, 60);
  EXPECT_EQ(buyer.lastPx, px(10'000));
  EXPECT_EQ(buyer.avgPx, px(10'000));

  EXPECT_EQ(seller.client, 1U);
  EXPECT_EQ(seller.clOrdId, "S1");
  EXPECT_EQ(seller.state, OrdState::PartiallyFilled);
  EXPECT_EQ(seller.cumQty, 60);
  EXPECT_EQ(seller.leavesQty, 40);
}

TEST_F(OrderManagerTest, RiskRejectCarriesReasonAndText) {
  send(order(1, "BIG", Side::Buy, 10'000, 1'001));
  const ExecutionReport& r = last();
  EXPECT_EQ(r.execType, ExecType::Rejected);
  EXPECT_EQ(r.state, OrdState::Rejected);
  EXPECT_EQ(r.ordRejReason, ord_rej_reason::OrderExceedsLimit);
  EXPECT_EQ(r.text, "QuantityAboveMax");
  EXPECT_NE(r.orderId, 0U); // the order exists, so its ClOrdID can't be reused
  EXPECT_EQ(r.leavesQty, 0);
  EXPECT_FALSE(venue.book(kStock)); // never reached the venue
}

TEST_F(OrderManagerTest, RiskRejectReasonsMapToFix) {
  ClientNewOrder halted = order(1, "H", Side::Buy, 10'000, 10);
  halted.symbol = kHalted;
  send(halted);
  EXPECT_EQ(last().ordRejReason, ord_rej_reason::ExchangeClosed);

  send(order(1, "BAND", Side::Buy, 20'000, 10));
  EXPECT_EQ(last().ordRejReason, ord_rej_reason::Other);
  EXPECT_EQ(last().text, "OutsidePriceBand");

  send(order(1, "ACCT", Side::Buy, 10'000, 10, TimeInForce::Day, /*account=*/9));
  EXPECT_EQ(last().ordRejReason, ord_rej_reason::UnknownAccount);
}

TEST_F(OrderManagerTest, InvalidRequestIsRejectedWithItsReason) {
  ClientNewOrder market = order(1, "MKT", Side::Buy, 10'000, 10);
  market.invalid = InvalidRequest{ord_rej_reason::UnsupportedOrderCharacteristic, "only limit orders"};
  send(market);
  EXPECT_EQ(last().execType, ExecType::Rejected);
  EXPECT_EQ(last().ordRejReason, ord_rej_reason::UnsupportedOrderCharacteristic);
  EXPECT_EQ(last().text, "only limit orders");
}

TEST_F(OrderManagerTest, DuplicateClOrdIdIsRejectedAndOriginalUntouched) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  send(order(1, "A", Side::Buy, 10'000, 500));
  const ExecutionReport& r = last();
  EXPECT_EQ(r.execType, ExecType::Rejected);
  EXPECT_EQ(r.ordRejReason, ord_rej_reason::DuplicateOrder);
  EXPECT_EQ(r.orderId, 0U); // no new order
  EXPECT_EQ(manager.orderCount(), 1U);
  EXPECT_EQ(venue.book(kStock)->depthAt(Side::Buy, px(10'000)), 100);
}

TEST_F(OrderManagerTest, PossResendOfKnownOrderOnlyReportsItsStatus) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  ClientNewOrder resend = order(1, "A", Side::Buy, 10'000, 100);
  resend.possResend = true;
  send(resend);
  EXPECT_EQ(last().execType, ExecType::OrderStatus);
  EXPECT_EQ(last().state, OrdState::New);
  EXPECT_EQ(last().orderId, sink.reports.front().orderId);
  EXPECT_EQ(manager.orderCount(), 1U);
  EXPECT_EQ(venue.book(kStock)->depthAt(Side::Buy, px(10'000)), 100); // not doubled
}

TEST_F(OrderManagerTest, PossResendReusingACancelsClOrdIdIsADuplicate) {
  // "C1" was a cancel request's ClOrdID, never a NewOrderSingle's, so this can't
  // be a resend of a new order. Found by the random-flow test below.
  send(order(1, "A", Side::Buy, 10'000, 100));
  send(order(1, "B", Side::Buy, 10'000, 100));
  cancel(1, "C1", "A");
  ClientNewOrder reuse = order(1, "C1", Side::Buy, 10'000, 100);
  reuse.possResend = true;
  send(reuse);
  EXPECT_EQ(last().execType, ExecType::Rejected);
  EXPECT_EQ(last().ordRejReason, ord_rej_reason::DuplicateOrder);
  EXPECT_EQ(last().clOrdId, "C1");
}

TEST_F(OrderManagerTest, PossResendOfUnknownOrderIsProcessed) {
  // The original never arrived, so the resend is the only copy.
  ClientNewOrder resend = order(1, "LOST", Side::Buy, 10'000, 100);
  resend.possResend = true;
  send(resend);
  EXPECT_EQ(last().execType, ExecType::New);
  EXPECT_EQ(manager.orderCount(), 1U);
}

TEST_F(OrderManagerTest, ClOrdIdsAreScopedToEachClient) {
  send(order(1, "A", Side::Buy, 10'000, 10));
  send(order(2, "A", Side::Buy, 10'000, 10));
  EXPECT_EQ(sink.reports[0].execType, ExecType::New);
  EXPECT_EQ(sink.reports[1].execType, ExecType::New);
  EXPECT_EQ(manager.orderCount(), 2U);
}

// --- Cancel ----------------------------------------------------------------------------

TEST_F(OrderManagerTest, CancelReportsBothClOrdIds) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  cancel(1, "C1", "A");
  const ExecutionReport& r = last();
  EXPECT_EQ(r.execType, ExecType::Canceled);
  EXPECT_EQ(r.state, OrdState::Canceled);
  EXPECT_EQ(r.clOrdId, "C1");
  EXPECT_EQ(r.origClOrdId, "A");
  EXPECT_EQ(r.leavesQty, 0);
  EXPECT_FALSE(venue.book(kStock)->bestBid().has_value());
}

TEST_F(OrderManagerTest, SecondCancelIsTooLate) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  cancel(1, "C1", "A");
  cancel(1, "C2", "C1");
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::TooLateToCancel);
  EXPECT_EQ(sink.rejects[0].state, OrdState::Canceled);
  EXPECT_FALSE(sink.rejects[0].replace);
}

TEST_F(OrderManagerTest, CancelOfFilledOrderIsTooLate) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  send(order(2, "S", Side::Sell, 10'000, 100));
  cancel(1, "C1", "A");
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::TooLateToCancel);
  EXPECT_EQ(sink.rejects[0].state, OrdState::Filled);
}

TEST_F(OrderManagerTest, CancelOfUnknownOrder) {
  cancel(1, "C1", "NOPE");
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::UnknownOrder);
  EXPECT_EQ(sink.rejects[0].orderId, 0U);
  EXPECT_EQ(sink.rejects[0].state, OrdState::Rejected);
}

TEST_F(OrderManagerTest, CancelWithDuplicateClOrdId) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  send(order(1, "B", Side::Buy, 10'000, 100));
  cancel(1, "B", "A"); // "B" is already taken
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::DuplicateClOrdId);
  EXPECT_EQ(venue.book(kStock)->depthAt(Side::Buy, px(10'000)), 200); // nothing cancelled
}

TEST_F(OrderManagerTest, ClientCannotCancelAnotherClientsOrder) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  cancel(2, "C1", "A"); // client 2 has no order "A"
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::UnknownOrder);
}

TEST_F(OrderManagerTest, CancelBeforeAcknowledgementSeenByClient) {
  // The client sends a new order and a cancel back to back. Requests are handled
  // in order, so the order is accepted first and the cancel finds it.
  send(order(1, "A", Side::Buy, 10'000, 100));
  cancel(1, "C1", "A");
  ASSERT_EQ(sink.reports.size(), 2U);
  EXPECT_EQ(sink.reports[0].execType, ExecType::New);
  EXPECT_EQ(sink.reports[1].execType, ExecType::Canceled);
}

// --- Replace ---------------------------------------------------------------------------

TEST_F(OrderManagerTest, ReplaceChangesPriceAndQuantity) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  replace(1, "R1", "A", 9'990, 150);
  const ExecutionReport& r = last();
  EXPECT_EQ(r.execType, ExecType::Replaced);
  EXPECT_EQ(r.state, OrdState::New);
  EXPECT_EQ(r.clOrdId, "R1");
  EXPECT_EQ(r.origClOrdId, "A");
  EXPECT_EQ(r.orderQty, 150);
  EXPECT_EQ(r.price, px(9'990));
  EXPECT_EQ(venue.book(kStock)->depthAt(Side::Buy, px(9'990)), 150);

  cancel(1, "C1", "R1"); // the order now goes by its new ClOrdID
  EXPECT_EQ(last().execType, ExecType::Canceled);
}

TEST_F(OrderManagerTest, ReplaceToCrossingPriceTrades) {
  send(order(2, "S", Side::Sell, 10'010, 50));
  send(order(1, "A", Side::Buy, 10'000, 50));
  sink.clear();
  replace(1, "R1", "A", 10'010, 50);
  ASSERT_EQ(sink.reports.size(), 3U);
  EXPECT_EQ(sink.reports[0].execType, ExecType::Replaced);
  EXPECT_EQ(sink.reports[1].execType, ExecType::Trade);
  EXPECT_EQ(sink.reports[1].clOrdId, "R1");
  EXPECT_EQ(sink.reports[1].state, OrdState::Filled);
  EXPECT_EQ(sink.reports[2].client, 2U);
}

TEST_F(OrderManagerTest, ReplaceCannotChangeSideOrSymbol) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  replace(1, "R1", "A", 10'000, 100, Side::Sell);
  replace(1, "R2", "A", 10'000, 100, Side::Buy, kHalted);
  ASSERT_EQ(sink.rejects.size(), 2U);
  EXPECT_TRUE(sink.rejects[0].replace);
  EXPECT_EQ(sink.rejects[0].reason, cxl_rej_reason::Other);
  EXPECT_EQ(sink.rejects[1].reason, cxl_rej_reason::Other);
  EXPECT_EQ(manager.order(sink.reports[0].orderId)->state(), OrdState::New); // untouched
}

TEST_F(OrderManagerTest, ReplaceIsRiskChecked) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  replace(1, "R1", "A", 20'000, 100); // outside the price band
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].text, "OutsidePriceBand");
  EXPECT_EQ(sink.rejects[0].state, OrdState::New);
  EXPECT_EQ(venue.book(kStock)->depthAt(Side::Buy, px(10'000)), 100); // order still resting as it was
}

TEST_F(OrderManagerTest, ReplaceBelowFilledQuantityIsRefused) {
  send(order(1, "A", Side::Buy, 10'000, 100));
  send(order(2, "S", Side::Sell, 10'000, 60)); // fills 60 of "A"
  replace(1, "R1", "A", 10'000, 50);
  ASSERT_EQ(sink.rejects.size(), 1U);
  EXPECT_EQ(sink.rejects[0].text, "InvalidQuantity");
  EXPECT_EQ(sink.rejects[0].state, OrdState::PartiallyFilled);
}

// --- IOC and risk exposure ----------------------------------------------------------------

TEST_F(OrderManagerTest, IocRemainderIsCancelled) {
  send(order(2, "S", Side::Sell, 10'000, 30));
  sink.clear();
  send(order(1, "I", Side::Buy, 10'000, 50, TimeInForce::Ioc));
  ASSERT_EQ(sink.reports.size(), 4U); // New, Trade (buyer), Trade (seller), Canceled
  EXPECT_EQ(sink.reports[3].execType, ExecType::Canceled);
  EXPECT_EQ(sink.reports[3].cumQty, 30);
  EXPECT_EQ(sink.reports[3].leavesQty, 0);
}

TEST_F(OrderManagerTest, ExposureFollowsTheOrders) {
  send(order(1, "A", Side::Buy, 10'000, 300));
  EXPECT_EQ(risk.exposure(kAccount, kStock).openBuy, 300);

  send(order(2, "S", Side::Sell, 10'000, 100));
  EXPECT_EQ(risk.exposure(kAccount, kStock).openBuy, 200);
  EXPECT_EQ(risk.exposure(kAccount, kStock).position, 0); // +100 bought, -100 sold: same account

  cancel(1, "C1", "A");
  EXPECT_EQ(risk.exposure(kAccount, kStock).openBuy, 0);
}

TEST_F(OrderManagerTest, OpenOrdersCountTowardsThePositionLimit) {
  send(order(1, "A", Side::Buy, 10'000, 300, TimeInForce::Day, kTight));
  send(order(1, "B", Side::Buy, 10'000, 300, TimeInForce::Day, kTight)); // 600 > 500
  EXPECT_EQ(last().execType, ExecType::Rejected);
  EXPECT_EQ(last().text, "PositionLimit");
  cancel(1, "C1", "A");
  send(order(1, "D", Side::Buy, 10'000, 300, TimeInForce::Day, kTight)); // room again
  EXPECT_EQ(last().execType, ExecType::New);
}

// --- Random flow ----------------------------------------------------------------------------

// 20,000 random requests from three clients: new orders (some with reused or
// resent ClOrdIDs), cancels and replaces (some for unknown or finished orders).
// Checks: the venue never sends an event the state machine refuses; every cancel
// or replace gets exactly one answer; every new order gets at least one report;
// and risk exposure always equals what the orders really hold.
TEST(OrderManagerModel, RandomFlowStaysConsistent) {
  RiskEngine risk({SymbolLimits{true, 200, px(1), px(10'000), 300}, SymbolLimits{true, 200, px(1), px(5'000), 300}},
                  {AccountLimits{notional(100'000), 1'000, notional(400'000)},
                   AccountLimits{notional(100'000), 1'000, notional(400'000)}});
  OrderManager manager(risk);
  InProcessVenue venue(manager);
  Recorder sink;
  manager.connect(venue, sink);

  std::mt19937 rng(20261011);
  std::uniform_int_distribution<int> pick(0, 99);
  std::uniform_int_distribution<std::int64_t> offset(-400, 400); // cents; the band is 3%
  std::uniform_int_distribution<Qty> qty(1, 250);
  std::map<ClientId, std::vector<std::string>> used; // ClOrdIDs each client has sent
  int nextId = 1;

  for (int step = 0; step < 20'000; ++step) {
    const auto client = static_cast<ClientId>(pick(rng) % 3);
    auto& ids = used[client];
    const auto anyId = [&]() { return ids.empty() ? std::string("none") : ids[static_cast<std::size_t>(pick(rng)) % ids.size()]; };
    const int choice = pick(rng);
    sink.clear();

    if (choice < 60) {
      ClientNewOrder o;
      o.client = client;
      o.clOrdId = choice < 5 ? anyId() : "N" + std::to_string(nextId++); // a few reuse an old ID
      o.possResend = choice < 2;
      o.account = static_cast<AccountId>(pick(rng) % 2);
      o.symbol = static_cast<SymbolId>(pick(rng) % 2);
      o.side = pick(rng) < 50 ? Side::Buy : Side::Sell;
      o.price = px((o.symbol == 0 ? 10'000 : 5'000) + offset(rng));
      o.qty = qty(rng);
      o.tif = pick(rng) < 20 ? TimeInForce::Ioc : TimeInForce::Day;
      manager.onNewOrder(o);
      ids.push_back(o.clOrdId);
      bool answered = false;
      for (const ExecutionReport& r : sink.reports) answered |= r.client == client && r.clOrdId == o.clOrdId;
      ASSERT_TRUE(answered) << "new order " << o.clOrdId << " unanswered, step " << step;
    } else {
      const std::string id = "Q" + std::to_string(nextId++);
      const std::string orig = anyId();
      if (choice < 80) {
        manager.onCancel({client, id, orig});
      } else {
        ClientReplace r;
        r.client = client;
        r.clOrdId = id;
        r.origClOrdId = orig;
        const OrderId target = manager.orderIdFor(client, orig);
        const Order* current = manager.order(target);
        r.symbol = current ? current->symbol() : 0;
        r.side = current ? current->side() : Side::Buy;
        r.price = px((r.symbol == 0 ? 10'000 : 5'000) + offset(rng));
        r.qty = qty(rng);
        manager.onReplace(r);
      }
      ids.push_back(id);
      int answers = static_cast<int>(sink.rejects.size());
      for (const ExecutionReport& rep : sink.reports) {
        if (rep.clOrdId == id && (rep.execType == ExecType::Canceled || rep.execType == ExecType::Replaced)) ++answers;
      }
      ASSERT_EQ(answers, 1) << "request " << id << " step " << step;
    }

    ASSERT_EQ(manager.venueAnomalies(), 0U) << "step " << step;
    if (step % 250 == 0) {
      // Recount exposure from the orders themselves.
      RiskEngine::Exposure expected[2][2] = {};
      for (OrderId oid = 1; const Order* o = manager.order(oid); ++oid) {
        RiskEngine::Exposure& e = expected[o->account()][o->symbol()];
        const Qty open = o->state() == OrdState::PendingNew ? 0 : o->leavesQty();
        (o->side() == Side::Buy ? e.openBuy : e.openSell) += open;
        e.position += o->side() == Side::Buy ? o->cumQty() : -o->cumQty();
      }
      for (AccountId a = 0; a < 2; ++a) {
        for (SymbolId s = 0; s < 2; ++s) {
          ASSERT_EQ(risk.exposure(a, s).openBuy, expected[a][s].openBuy) << "step " << step;
          ASSERT_EQ(risk.exposure(a, s).openSell, expected[a][s].openSell) << "step " << step;
          ASSERT_EQ(risk.exposure(a, s).position, expected[a][s].position) << "step " << step;
        }
      }
    }
  }
}

} // namespace
} // namespace oems
