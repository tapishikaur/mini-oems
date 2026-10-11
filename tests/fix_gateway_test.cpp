// End-to-end FIX tests: the gateway runs under QuickFIX's acceptor, a QuickFIX
// initiator plays the client, and they talk over a real localhost socket. Every
// assertion is about FIX messages on the wire.

#include "oems/config.hpp"
#include "oems/fix_gateway.hpp"
#include "oems/in_process_venue.hpp"
#include "oems/order_manager.hpp"
#include "oems/risk.hpp"

#include <gtest/gtest.h>

#include <quickfix/Application.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixFields.h>
#include <quickfix/Log.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/Utility.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelReplaceRequest.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/OrderStatusRequest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace oems {
namespace {

using namespace std::chrono_literals;
using Match = std::function<bool(const FIX::Message&)>;

std::string get(const FIX::FieldMap& m, int tag) { return m.isSetField(tag) ? m.getField(tag) : ""; }
std::string msgType(const FIX::Message& m) { return get(m.getHeader(), FIX::FIELD::MsgType); }

Match report(const std::string& clOrdId, char execType) {
  return [=](const FIX::Message& m) {
    return msgType(m) == "8" && get(m, FIX::FIELD::ClOrdID) == clOrdId &&
           get(m, FIX::FIELD::ExecType) == std::string(1, execType);
  };
}
Match cancelReject(const std::string& clOrdId) {
  return [=](const FIX::Message& m) { return msgType(m) == "9" && get(m, FIX::FIELD::ClOrdID) == clOrdId; };
}

// A FIX client for tests: keeps every message it receives and can wait for one.
class TestClient : public FIX::Application {
public:
  void onCreate(const FIX::SessionID& s) override { session_ = s; }
  void onLogon(const FIX::SessionID&) override { update([&] { loggedOn_ = true; }); }
  void onLogout(const FIX::SessionID&) override { update([&] { loggedOn_ = false; }); }
  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void toApp(FIX::Message&, const FIX::SessionID&) override {}
  void fromAdmin(const FIX::Message& m, const FIX::SessionID&) override { update([&] { admin_.push_back(m); }); }
  void fromApp(const FIX::Message& m, const FIX::SessionID&) override { update([&] { app_.push_back(m); }); }

  bool waitForLogon(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return loggedOn_; });
  }

  // The first application message that matches, waiting up to `timeout` for one.
  std::optional<FIX::Message> waitFor(const Match& match, std::chrono::milliseconds timeout = 5s) {
    std::unique_lock lock(mutex_);
    std::optional<FIX::Message> found;
    changed_.wait_for(lock, timeout, [&] {
      const auto it = std::find_if(app_.begin(), app_.end(), match);
      if (it != app_.end()) found = *it;
      return found.has_value();
    });
    return found;
  }

  std::size_t count(const Match& match) {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(app_.begin(), app_.end(), match));
  }
  bool receivedAdmin(const std::string& type) {
    std::lock_guard lock(mutex_);
    return std::any_of(admin_.begin(), admin_.end(), [&](const FIX::Message& m) { return msgType(m) == type; });
  }
  const FIX::SessionID& session() const { return session_; }

private:
  template <typename Change> void update(Change change) {
    {
      std::lock_guard lock(mutex_);
      change();
    }
    changed_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool loggedOn_ = false;
  std::vector<FIX::Message> app_;
  std::vector<FIX::Message> admin_;
  FIX::SessionID session_;
};

// A port nobody is listening on right now.
int freePort() {
  FIX::socket_init();
  const FIX::socket_handle s = FIX::socket_createAcceptor(0, true);
  // QuickFIX returns the port as a short, so ports above 32767 come back negative.
  const auto port = static_cast<unsigned short>(FIX::socket_hostport(s));
  FIX::socket_close(s);
  return port;
}

// Set MINI_OEMS_FIX_LOG=1 to print every FIX message both sides send and receive.
bool fixLogging() { return std::getenv("MINI_OEMS_FIX_LOG") != nullptr; }
FIX::ScreenLogFactory screenLog(true, true, true);

std::string sessionSettings(const char* role, int port, const char* sender, const char* target) {
  std::ostringstream s;
  s << "[DEFAULT]\nConnectionType=" << role << "\nStartTime=00:00:00\nEndTime=00:00:00\nHeartBtInt=30\n"
    << "UseDataDictionary=Y\nDataDictionary=" << MINI_OEMS_FIX44_XML << "\nResetOnLogon=Y\n"
    << "ReconnectInterval=1\nSocketReuseAddress=Y\nSocketAcceptPort=" << port
    << "\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" << port << "\n"
    << "[SESSION]\nBeginString=FIX.4.4\nSenderCompID=" << sender << "\nTargetCompID=" << target << "\n";
  return s.str();
}

// Everything on the gateway side, built the way main.cpp builds it.
struct Gateway {
  explicit Gateway(int port)
      : instruments(load<InstrumentConfig>(instrumentsCsv(), loadInstruments)),
        accounts(load<AccountConfig>("ACC, 10000000, 1000000, 1000000000\n", loadAccounts)),
        risk(instruments.limits, accounts.limits),
        manager(risk),
        venue(manager),
        fix(manager, instruments.symbols, accounts.accounts),
        settings(makeSettings(sessionSettings("acceptor", port, "OEMS", "CLIENT"))),
        acceptor(fixLogging() ? std::make_unique<FIX::SocketAcceptor>(fix, store, settings, screenLog)
                              : std::make_unique<FIX::SocketAcceptor>(fix, store, settings)) {
    manager.connect(venue, fix);
  }

  // One symbol per test (T1, T2, ...), so tests never trade with each other.
  static std::string instrumentsCsv() {
    std::string csv;
    for (int i = 1; i <= 20; ++i) csv += "T" + std::to_string(i) + ", 0.01, 100.00, 500, 1000, Y\n";
    return csv;
  }
  template <typename Config, typename Load> static Config load(const std::string& text, Load loader) {
    std::istringstream in(text);
    return loader(in);
  }
  static FIX::SessionSettings makeSettings(const std::string& text) {
    std::istringstream in(text);
    return FIX::SessionSettings(in);
  }

  InstrumentConfig instruments;
  AccountConfig accounts;
  RiskEngine risk;
  OrderManager manager;
  InProcessVenue venue;
  FixGateway fix;
  FIX::SessionSettings settings;
  FIX::MemoryStoreFactory store;
  std::unique_ptr<FIX::SocketAcceptor> acceptor;
};

class FixGatewayTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    const int port = freePort();
    gateway = std::make_unique<Gateway>(port);
    std::istringstream clientSettings(sessionSettings("initiator", port, "CLIENT", "OEMS"));
    initiatorSettings = std::make_unique<FIX::SessionSettings>(clientSettings);
    client = std::make_unique<TestClient>();
    initiator = fixLogging() ? std::make_unique<FIX::SocketInitiator>(*client, clientStore, *initiatorSettings, screenLog)
                             : std::make_unique<FIX::SocketInitiator>(*client, clientStore, *initiatorSettings);
    gateway->acceptor->start();
    initiator->start();
    ASSERT_TRUE(client->waitForLogon(15s)) << "the test client never logged on";
  }

  static void TearDownTestSuite() {
    initiator->stop();
    gateway->acceptor->stop();
    initiator.reset();
    client.reset();
    initiatorSettings.reset();
    gateway.reset();
  }

  // A limit NewOrderSingle for account ACC. `change` can alter it before sending.
  static void sendNew(const std::string& clOrdId, const std::string& symbol, char side, const std::string& price,
                      const std::string& qty, const std::function<void(FIX::Message&)>& change = {}) {
    FIX44::NewOrderSingle m;
    m.setField(FIX::FIELD::ClOrdID, clOrdId);
    m.setField(FIX::FIELD::Account, "ACC");
    m.setField(FIX::FIELD::Symbol, symbol);
    m.setField(FIX::FIELD::Side, std::string(1, side));
    m.setField(FIX::TransactTime());
    m.setField(FIX::FIELD::OrdType, "2");
    m.setField(FIX::FIELD::Price, price);
    m.setField(FIX::FIELD::OrderQty, qty);
    m.setField(FIX::FIELD::TimeInForce, "0");
    if (change) change(m);
    send(m);
  }

  static void sendCancel(const std::string& clOrdId, const std::string& orig, const std::string& symbol, char side) {
    FIX44::OrderCancelRequest m;
    m.setField(FIX::FIELD::ClOrdID, clOrdId);
    m.setField(FIX::FIELD::OrigClOrdID, orig);
    m.setField(FIX::FIELD::Symbol, symbol);
    m.setField(FIX::FIELD::Side, std::string(1, side));
    m.setField(FIX::TransactTime());
    send(m);
  }

  static void sendReplace(const std::string& clOrdId, const std::string& orig, const std::string& symbol, char side,
                          const std::string& price, const std::string& qty) {
    FIX44::OrderCancelReplaceRequest m;
    m.setField(FIX::FIELD::ClOrdID, clOrdId);
    m.setField(FIX::FIELD::OrigClOrdID, orig);
    m.setField(FIX::FIELD::Account, "ACC");
    m.setField(FIX::FIELD::Symbol, symbol);
    m.setField(FIX::FIELD::Side, std::string(1, side));
    m.setField(FIX::TransactTime());
    m.setField(FIX::FIELD::OrdType, "2");
    m.setField(FIX::FIELD::Price, price);
    m.setField(FIX::FIELD::OrderQty, qty);
    send(m);
  }

  static void send(FIX::Message& m) { FIX::Session::sendToTarget(m, client->session()); }

  static inline std::unique_ptr<Gateway> gateway;
  static inline std::unique_ptr<TestClient> client;
  static inline FIX::MemoryStoreFactory clientStore;
  static inline std::unique_ptr<FIX::SessionSettings> initiatorSettings;
  static inline std::unique_ptr<FIX::SocketInitiator> initiator;
};

// --- New orders --------------------------------------------------------------------------

TEST_F(FixGatewayTest, NewOrderIsAcknowledgedWithExactValues) {
  sendNew("N1", "T1", '1', "100.25", "100");
  const auto r = client->waitFor(report("N1", '0'));
  ASSERT_TRUE(r) << "no ExecutionReport";
  EXPECT_EQ(get(*r, FIX::FIELD::OrdStatus), "0");
  EXPECT_EQ(get(*r, FIX::FIELD::Price), "100.25"); // exactly as sent: no double in between
  EXPECT_EQ(get(*r, FIX::FIELD::OrderQty), "100");
  EXPECT_EQ(get(*r, FIX::FIELD::LeavesQty), "100");
  EXPECT_EQ(get(*r, FIX::FIELD::CumQty), "0");
  EXPECT_EQ(get(*r, FIX::FIELD::Symbol), "T1");
  EXPECT_EQ(get(*r, FIX::FIELD::Account), "ACC");
  EXPECT_NE(get(*r, FIX::FIELD::OrderID), "NONE");
}

TEST_F(FixGatewayTest, CrossingOrdersTradeAtTheRestingPrice) {
  sendNew("X1", "T2", '1', "100", "50");
  ASSERT_TRUE(client->waitFor(report("X1", '0')));
  sendNew("X2", "T2", '2', "99.95", "50"); // willing to sell lower than the bid
  const auto seller = client->waitFor(report("X2", 'F'));
  const auto buyer = client->waitFor(report("X1", 'F'));
  ASSERT_TRUE(seller && buyer);
  EXPECT_EQ(get(*seller, FIX::FIELD::LastPx), "100"); // the resting price
  EXPECT_EQ(get(*seller, FIX::FIELD::LastQty), "50");
  EXPECT_EQ(get(*seller, FIX::FIELD::OrdStatus), "2");
  EXPECT_EQ(get(*seller, FIX::FIELD::AvgPx), "100");
  EXPECT_EQ(get(*buyer, FIX::FIELD::OrdStatus), "2");
  EXPECT_EQ(get(*buyer, FIX::FIELD::LeavesQty), "0");
}

TEST_F(FixGatewayTest, RiskRejectHasFixReasonAndText) {
  sendNew("RJ1", "T3", '1', "100", "1001"); // the per-order maximum is 1,000
  const auto r = client->waitFor(report("RJ1", '8'));
  ASSERT_TRUE(r);
  EXPECT_EQ(get(*r, FIX::FIELD::OrdStatus), "8");
  EXPECT_EQ(get(*r, FIX::FIELD::OrdRejReason), "3"); // order exceeds limit
  EXPECT_EQ(get(*r, FIX::FIELD::Text), "QuantityAboveMax");
}

TEST_F(FixGatewayTest, MarketOrdersAreRejectedAsUnsupported) {
  sendNew("MK1", "T4", '1', "100", "10", [](FIX::Message& m) {
    m.setField(FIX::FIELD::OrdType, "1");
    m.removeField(FIX::FIELD::Price);
  });
  const auto r = client->waitFor(report("MK1", '8'));
  ASSERT_TRUE(r);
  EXPECT_EQ(get(*r, FIX::FIELD::OrdRejReason), "11"); // unsupported order characteristic
}

TEST_F(FixGatewayTest, UnknownAccountAndSymbolAreRejected) {
  sendNew("UA1", "T4", '1', "100", "10", [](FIX::Message& m) { m.setField(FIX::FIELD::Account, "NOPE"); });
  sendNew("US1", "ZZZZ", '1', "100", "10");
  const auto account = client->waitFor(report("UA1", '8'));
  const auto symbol = client->waitFor(report("US1", '8'));
  ASSERT_TRUE(account && symbol);
  EXPECT_EQ(get(*account, FIX::FIELD::OrdRejReason), "15");
  EXPECT_EQ(get(*symbol, FIX::FIELD::OrdRejReason), "1");
  EXPECT_FALSE(symbol->isSetField(FIX::FIELD::Symbol)); // not echoed: we don't know it
}

TEST_F(FixGatewayTest, PriceWithTooManyDecimalsIsRejectedNotRounded) {
  sendNew("PX1", "T5", '1', "100.12345", "10");
  const auto r = client->waitFor(report("PX1", '8'));
  ASSERT_TRUE(r);
  EXPECT_EQ(get(*r, FIX::FIELD::OrdRejReason), "99");
  EXPECT_NE(get(*r, FIX::FIELD::Text).find("at most 4 decimal places"), std::string::npos);
}

TEST_F(FixGatewayTest, DuplicateClOrdIdIsRejected) {
  sendNew("D1", "T7", '1', "100", "10");
  ASSERT_TRUE(client->waitFor(report("D1", '0')));
  sendNew("D1", "T7", '1', "100", "10");
  const auto r = client->waitFor(report("D1", '8'));
  ASSERT_TRUE(r);
  EXPECT_EQ(get(*r, FIX::FIELD::OrdRejReason), "6");
  EXPECT_EQ(get(*r, FIX::FIELD::OrderID), "NONE"); // no second order was created
  EXPECT_EQ(client->count(report("D1", '0')), 1U);
}

TEST_F(FixGatewayTest, PossResendOfKnownOrderReportsItsStatus) {
  sendNew("P1", "T8", '1', "100", "10");
  ASSERT_TRUE(client->waitFor(report("P1", '0')));
  sendNew("P1", "T8", '1', "100", "10", [](FIX::Message& m) { m.getHeader().setField(FIX::PossResend(true)); });
  const auto r = client->waitFor(report("P1", 'I'));
  ASSERT_TRUE(r) << "expected an order status report";
  EXPECT_EQ(get(*r, FIX::FIELD::OrdStatus), "0");
  EXPECT_EQ(client->count(report("P1", '0')), 1U);
}

// --- Cancel and replace ----------------------------------------------------------------------

TEST_F(FixGatewayTest, CancelThenSecondCancelIsTooLate) {
  sendNew("C1", "T6", '1', "100", "10");
  ASSERT_TRUE(client->waitFor(report("C1", '0')));
  sendCancel("C2", "C1", "T6", '1');
  const auto done = client->waitFor(report("C2", '4'));
  ASSERT_TRUE(done);
  EXPECT_EQ(get(*done, FIX::FIELD::OrigClOrdID), "C1");
  EXPECT_EQ(get(*done, FIX::FIELD::OrdStatus), "4");
  EXPECT_EQ(get(*done, FIX::FIELD::LeavesQty), "0");

  sendCancel("C3", "C2", "T6", '1');
  const auto reject = client->waitFor(cancelReject("C3"));
  ASSERT_TRUE(reject);
  EXPECT_EQ(get(*reject, FIX::FIELD::CxlRejReason), "0");     // too late
  EXPECT_EQ(get(*reject, FIX::FIELD::CxlRejResponseTo), "1"); // answers a cancel
  EXPECT_EQ(get(*reject, FIX::FIELD::OrdStatus), "4");
}

TEST_F(FixGatewayTest, CancelOfUnknownOrder) {
  sendCancel("UC1", "NEVER-SENT", "T6", '1');
  const auto reject = client->waitFor(cancelReject("UC1"));
  ASSERT_TRUE(reject);
  EXPECT_EQ(get(*reject, FIX::FIELD::CxlRejReason), "1");
  EXPECT_EQ(get(*reject, FIX::FIELD::OrderID), "NONE");
}

TEST_F(FixGatewayTest, CancelSentBeforeTheAckArrives) {
  // Back to back, without waiting: the gateway handles them in order.
  sendNew("CB1", "T11", '1', "100", "10");
  sendCancel("CB2", "CB1", "T11", '1');
  const auto acked = client->waitFor(report("CB1", '0'));
  const auto cancelled = client->waitFor(report("CB2", '4'));
  ASSERT_TRUE(acked && cancelled);
  EXPECT_LT(std::stoull(get(*acked, FIX::FIELD::ExecID)), std::stoull(get(*cancelled, FIX::FIELD::ExecID)));
}

TEST_F(FixGatewayTest, ReplaceChangesPriceAndQuantity) {
  sendNew("R1", "T9", '1', "100", "100");
  ASSERT_TRUE(client->waitFor(report("R1", '0')));
  sendReplace("R2", "R1", "T9", '1', "99.50", "150");
  const auto r = client->waitFor(report("R2", '5'));
  ASSERT_TRUE(r);
  EXPECT_EQ(get(*r, FIX::FIELD::OrigClOrdID), "R1");
  EXPECT_EQ(get(*r, FIX::FIELD::OrderQty), "150");
  EXPECT_EQ(get(*r, FIX::FIELD::Price), "99.5");
  EXPECT_EQ(get(*r, FIX::FIELD::OrdStatus), "0"); // FIX 4.4: status is the live state, not "replaced"
}

TEST_F(FixGatewayTest, ReplaceBelowFilledQuantityIsRejected) {
  sendNew("RB1", "T10", '1', "100", "100");
  ASSERT_TRUE(client->waitFor(report("RB1", '0')));
  sendNew("RB2", "T10", '2', "100", "60"); // fills 60 of RB1
  ASSERT_TRUE(client->waitFor(report("RB1", 'F')));
  sendReplace("RB3", "RB1", "T10", '1', "100", "50");
  const auto reject = client->waitFor(cancelReject("RB3"));
  ASSERT_TRUE(reject);
  EXPECT_EQ(get(*reject, FIX::FIELD::CxlRejResponseTo), "2"); // answers a cancel/replace
  EXPECT_EQ(get(*reject, FIX::FIELD::Text), "InvalidQuantity");
  EXPECT_EQ(get(*reject, FIX::FIELD::OrdStatus), "1");
}

TEST_F(FixGatewayTest, UnsupportedMessageGetsBusinessReject) {
  FIX44::OrderStatusRequest m;
  m.setField(FIX::FIELD::ClOrdID, "OS1");
  m.setField(FIX::FIELD::Symbol, "T1");
  m.setField(FIX::FIELD::Side, "1");
  send(m);
  const auto reject = client->waitFor([](const FIX::Message& r) { return msgType(r) == "j"; });
  ASSERT_TRUE(reject);
  EXPECT_EQ(get(*reject, FIX::FIELD::RefMsgType), "H");
}

// --- Session layer -----------------------------------------------------------------------------
// These change sequence numbers, so they run last.

TEST_F(FixGatewayTest, ResentOrderMarkedPossDupIsNotDuplicated) {
  // A real resend. The gateway is made to believe it never received PD1, so when
  // the next message arrives it asks the client to resend, and the client's
  // session layer sends PD1 again with PossDupFlag=Y. That copy reaches the
  // application in sequence. It must not become a second order, and it must not
  // be rejected as a duplicate either: it gets a status report.
  FIX::Session* gatewaySession = FIX::Session::lookupSession(FIX::SessionID("FIX.4.4", "OEMS", "CLIENT"));
  ASSERT_NE(gatewaySession, nullptr);
  sendNew("PD1", "T12", '1', "100", "10");
  ASSERT_TRUE(client->waitFor(report("PD1", '0')));

  gatewaySession->setNextTargetMsgSeqNum(gatewaySession->getExpectedTargetNum() - 1);
  sendNew("PD2", "T12", '1', "100", "10"); // arrives "early": the gateway asks for a resend

  ASSERT_TRUE(client->waitFor(report("PD2", '0'), 10s));
  ASSERT_TRUE(client->waitFor(report("PD1", 'I'))) << "the resent PD1 should get a status report";
  EXPECT_TRUE(client->receivedAdmin("2")) << "the gateway should have sent a ResendRequest";
  EXPECT_EQ(client->count(report("PD1", '0')), 1U); // still one order
  EXPECT_EQ(client->count(report("PD1", '8')), 0U); // and not rejected
  EXPECT_EQ(client->count(report("PD2", '0')), 1U); // PD2, sent twice by the session layer, handled once
}

TEST_F(FixGatewayTest, SequenceGapIsRecoveredInOrder) {
  // The gateway is made to believe the last two messages (G1, G2) were lost on
  // the way. G3 then arrives out of sequence. The gateway must hold G3, ask for
  // a resend, process the resent G1 and G2 first, and only then G3, once.
  //
  // (Faking the gap from the client side, by making it skip sequence numbers,
  // doesn't work: a real sender never skips its own numbers, and QuickFIX's
  // MemoryStore::get() looks up the first requested number exactly, so it
  // gap-fills the whole range instead of resending.)
  FIX::Session* gatewaySession = FIX::Session::lookupSession(FIX::SessionID("FIX.4.4", "OEMS", "CLIENT"));
  ASSERT_NE(gatewaySession, nullptr);
  sendNew("G1", "T13", '1', "100", "10");
  sendNew("G2", "T13", '1', "100", "10");
  ASSERT_TRUE(client->waitFor(report("G1", '0')));
  ASSERT_TRUE(client->waitFor(report("G2", '0')));

  gatewaySession->setNextTargetMsgSeqNum(gatewaySession->getExpectedTargetNum() - 2);
  sendNew("G3", "T13", '1', "100", "10");

  const auto g3 = client->waitFor(report("G3", '0'), 10s);
  const auto g1Again = client->waitFor(report("G1", 'I'));
  const auto g2Again = client->waitFor(report("G2", 'I'));
  ASSERT_TRUE(g3 && g1Again && g2Again);
  EXPECT_TRUE(client->receivedAdmin("2")) << "the gateway should have sent a ResendRequest";
  const auto execId = [](const FIX::Message& m) { return std::stoull(get(m, FIX::FIELD::ExecID)); };
  EXPECT_LT(execId(*g1Again), execId(*g2Again)); // resent messages in their original order...
  EXPECT_LT(execId(*g2Again), execId(*g3));      // ...before the one that arrived early
  EXPECT_EQ(client->count(report("G3", '0')), 1U);
  EXPECT_EQ(client->count(report("G1", '0')), 1U);
  EXPECT_EQ(client->count(report("G2", '0')), 1U);
}

} // namespace
} // namespace oems
