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
  void onNewOrderSingle(const FIX::Message& message, ClientId client);
  void onCancelRequest(const FIX::Message& message, ClientId client);
  void onReplaceRequest(const FIX::Message& message, ClientId client);
  void send(FIX::Message& message, ClientId client);

  OrderManager& orders_;
  const Directory& symbols_;
  const Directory& accounts_;
  std::map<FIX::SessionID, ClientId> clients_;
  std::vector<FIX::SessionID> sessions_; // indexed by ClientId
};

} // namespace oems
