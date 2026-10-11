#pragma once
// The FIX 4.4 edge. QuickFIX runs the session layer: logon, heartbeats,
// sequence numbers, and recovering lost messages (gap fill, resends). This class
// translates the application messages between FIX and the order manager:
//   in:  NewOrderSingle (35=D), OrderCancelRequest (35=F), OrderCancelReplaceRequest (35=G)
//   out: ExecutionReport (35=8), OrderCancelReject (35=9)
// Anything else is answered by QuickFIX with a BusinessMessageReject (35=j).
//
// Prices and quantities are read from the raw FIX text and parsed exactly; no
// value ever passes through a double.
//
// Run it under QuickFIX's SocketAcceptor, which calls every session from one
// thread, as the order manager requires.

#include "oems/directory.hpp"
#include "oems/order_manager.hpp"

#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/SessionID.h>

#include <map>
#include <vector>

namespace oems {

// The translations themselves, as free functions so they can be tested and
// benchmarked without a live session. A request that can't become a valid order
// (unknown symbol, unsupported type, bad price text...) comes back with
// `invalid` set, for the order manager to reject.
ClientNewOrder toClientNewOrder(const FIX::Message& m, ClientId client, const Directory& symbols,
                                const Directory& accounts);
ClientReplace toClientReplace(const FIX::Message& m, ClientId client, const Directory& symbols);
FIX::Message toFix(const ExecutionReport& report, const Directory& symbols, const Directory& accounts);
FIX::Message toFix(const CancelReject& reject);

class FixGateway final : public FIX::Application, public ReportSink {
public:
  FixGateway(OrderManager& orders, const Directory& symbols, const Directory& accounts)
      : orders_(orders), symbols_(symbols), accounts_(accounts) {}

  // FIX::Application
  void onCreate(const FIX::SessionID& session) override { clientFor(session); }
  void onLogon(const FIX::SessionID&) override {}
  void onLogout(const FIX::SessionID&) override {}
  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void toApp(FIX::Message&, const FIX::SessionID&) override {}
  void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
  void fromApp(const FIX::Message& message, const FIX::SessionID& session) override;

  // ReportSink
  void onExecutionReport(const ExecutionReport& report) override;
  void onCancelReject(const CancelReject& reject) override;

private:
  ClientId clientFor(const FIX::SessionID& session);
  void send(FIX::Message& message, ClientId client);

  OrderManager& orders_;
  const Directory& symbols_;
  const Directory& accounts_;
  std::map<FIX::SessionID, ClientId> clients_;
  std::vector<FIX::SessionID> sessions_; // indexed by ClientId
};

} // namespace oems
