#include "oems/fix_gateway.hpp"

#include <quickfix/Exceptions.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixFields.h>
#include <quickfix/FixValues.h>
#include <quickfix/Session.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/OrderCancelReject.h>

#include <optional>
#include <string>

namespace oems {
namespace {

std::optional<std::string> optionalField(const FIX::FieldMap& fields, int tag) {
  if (!fields.isSetField(tag)) return std::nullopt;
  return fields.getField(tag);
}

std::string orderIdText(OrderId id) { return id == 0 ? "NONE" : std::to_string(id); }

// Collects the first problem with a request; later problems are ignored, so
// the client hears about the first thing that's wrong.
class Problems {
public:
  void add(int reason, std::string text) {
    if (!first_) first_ = InvalidRequest{reason, std::move(text)};
  }
  std::optional<InvalidRequest> take() { return std::move(first_); }

private:
  std::optional<InvalidRequest> first_;
};

// The fields NewOrderSingle and OrderCancelReplaceRequest share.
struct OrderFields {
  SymbolId symbol = kNoId;
  Side side = Side::Buy;
  Price price;
  Qty qty = 0;
  TimeInForce tif = TimeInForce::Day;
};

// The reject code for each kind of problem. New orders are answered with an
// ExecutionReport (OrdRejReason), replaces with an OrderCancelReject (CxlRejReason).
struct Reasons {
  int unknownSymbol;
  int unsupported;
  int badQuantity;
  int badPrice;
};
constexpr Reasons kNewOrderReasons{ord_rej_reason::UnknownSymbol, ord_rej_reason::UnsupportedOrderCharacteristic,
                                   ord_rej_reason::IncorrectQuantity, ord_rej_reason::Other};
constexpr Reasons kReplaceReasons{cxl_rej_reason::Other, cxl_rej_reason::Other, cxl_rej_reason::Other,
                                  cxl_rej_reason::Other};

OrderFields readOrderFields(const FIX::Message& m, const Directory& symbols, Problems& problems,
                            const Reasons& reasons) {
  OrderFields f;
  const std::optional<std::string> symbol = optionalField(m, FIX::FIELD::Symbol);
  if (const auto id = symbol ? symbols.find(*symbol) : std::nullopt) {
    f.symbol = *id;
  } else {
    problems.add(reasons.unknownSymbol, symbol ? "unknown symbol " + *symbol : "missing Symbol");
  }

  const std::string& side = m.getField(FIX::FIELD::Side); // required by the data dictionary
  if (side == "1") {
    f.side = Side::Buy;
  } else if (side == "2") {
    f.side = Side::Sell;
  } else {
    problems.add(reasons.unsupported, "only Side=1 (buy) and Side=2 (sell) are supported");
  }

  if (m.getField(FIX::FIELD::OrdType) != "2") {
    problems.add(reasons.unsupported, "only limit orders (OrdType=2) are supported");
  }

  const std::optional<std::string> tif = optionalField(m, FIX::FIELD::TimeInForce);
  if (!tif || *tif == "0") {
    f.tif = TimeInForce::Day;
  } else if (*tif == "3") {
    f.tif = TimeInForce::Ioc;
  } else {
    problems.add(reasons.unsupported, "only TimeInForce=0 (day) and 3 (IOC) are supported");
  }

  const std::optional<std::string> qty = optionalField(m, FIX::FIELD::OrderQty);
  if (const std::optional<Qty> q = qty ? parseQty(*qty) : std::nullopt) {
    f.qty = *q;
  } else {
    problems.add(reasons.badQuantity, qty ? "OrderQty must be a whole number: " + *qty : "missing OrderQty");
  }

  const std::optional<std::string> price = optionalField(m, FIX::FIELD::Price);
  if (const std::optional<Price> p = price ? parsePrice(*price) : std::nullopt) {
    f.price = *p;
  } else {
    problems.add(reasons.badPrice,
                 price ? "Price must have at most 4 decimal places: " + *price : "a limit order needs a Price");
  }
  return f;
}

} // namespace

void FixGateway::fromApp(const FIX::Message& message, const FIX::SessionID& session) {
  const std::string& type = message.getHeader().getField(FIX::FIELD::MsgType);
  const ClientId client = clientFor(session);
  if (type == FIX::MsgType_NewOrderSingle) {
    onNewOrderSingle(message, client);
  } else if (type == FIX::MsgType_OrderCancelRequest) {
    onCancelRequest(message, client);
  } else if (type == FIX::MsgType_OrderCancelReplaceRequest) {
    onReplaceRequest(message, client);
  } else {
    throw FIX::UnsupportedMessageType(); // QuickFIX answers with a BusinessMessageReject
  }
}

void FixGateway::onNewOrderSingle(const FIX::Message& m, ClientId client) {
  ClientNewOrder order;
  order.client = client;
  order.clOrdId = m.getField(FIX::FIELD::ClOrdID);
  // Either flag means "this may repeat something you already have". PossDupFlag
  // copies usually stop at QuickFIX (sequence number too low), but one can arrive
  // in sequence, for instance when the gateway itself asked for a resend.
  const FIX::Header& header = m.getHeader();
  order.possResend = optionalField(header, FIX::FIELD::PossResend) == std::optional<std::string>("Y") ||
                     optionalField(header, FIX::FIELD::PossDupFlag) == std::optional<std::string>("Y");

  Problems problems;
  const std::optional<std::string> account = optionalField(m, FIX::FIELD::Account);
  if (const auto id = account ? accounts_.find(*account) : std::nullopt) {
    order.account = *id;
  } else {
    order.account = kNoId;
    problems.add(ord_rej_reason::UnknownAccount, account ? "unknown account " + *account : "missing Account");
  }

  const OrderFields f = readOrderFields(m, symbols_, problems, kNewOrderReasons);
  order.symbol = f.symbol;
  order.side = f.side;
  order.price = f.price;
  order.qty = f.qty;
  order.tif = f.tif;
  order.invalid = problems.take();
  orders_.onNewOrder(order);
}

void FixGateway::onCancelRequest(const FIX::Message& m, ClientId client) {
  orders_.onCancel({client, m.getField(FIX::FIELD::ClOrdID), m.getField(FIX::FIELD::OrigClOrdID)});
}

void FixGateway::onReplaceRequest(const FIX::Message& m, ClientId client) {
  ClientReplace replace;
  replace.client = client;
  replace.clOrdId = m.getField(FIX::FIELD::ClOrdID);
  replace.origClOrdId = m.getField(FIX::FIELD::OrigClOrdID);

  Problems problems;
  const OrderFields f = readOrderFields(m, symbols_, problems, kReplaceReasons);
  replace.symbol = f.symbol;
  replace.side = f.side;
  replace.price = f.price;
  replace.qty = f.qty;
  replace.invalid = problems.take();
  orders_.onReplace(replace);
}

void FixGateway::onExecutionReport(const ExecutionReport& r) {
  FIX44::ExecutionReport m;
  m.setField(FIX::FIELD::OrderID, orderIdText(r.orderId));
  m.setField(FIX::FIELD::ExecID, std::to_string(r.execId));
  m.setField(FIX::FIELD::ExecType, std::string(1, static_cast<char>(r.execType)));
  m.setField(FIX::FIELD::OrdStatus, std::string(1, fixOrdStatus(r.state)));
  m.setField(FIX::FIELD::ClOrdID, r.clOrdId);
  if (!r.origClOrdId.empty()) m.setField(FIX::FIELD::OrigClOrdID, r.origClOrdId);
  // Omitted when the request named an account or symbol we don't know.
  if (accounts_.contains(r.account)) m.setField(FIX::FIELD::Account, accounts_.name(r.account));
  if (symbols_.contains(r.symbol)) m.setField(FIX::FIELD::Symbol, symbols_.name(r.symbol));
  m.setField(FIX::FIELD::Side, r.side == Side::Buy ? "1" : "2");
  m.setField(FIX::FIELD::OrdType, "2");
  m.setField(FIX::FIELD::TimeInForce, r.tif == TimeInForce::Day ? "0" : "3");
  m.setField(FIX::FIELD::OrderQty, std::to_string(r.orderQty));
  m.setField(FIX::FIELD::Price, formatPrice(r.price));
  if (r.execType == ExecType::Trade) {
    m.setField(FIX::FIELD::LastQty, std::to_string(r.lastQty));
    m.setField(FIX::FIELD::LastPx, formatPrice(r.lastPx));
  }
  m.setField(FIX::FIELD::LeavesQty, std::to_string(r.leavesQty));
  m.setField(FIX::FIELD::CumQty, std::to_string(r.cumQty));
  m.setField(FIX::FIELD::AvgPx, formatPrice(r.avgPx));
  if (r.ordRejReason >= 0) m.setField(FIX::FIELD::OrdRejReason, std::to_string(r.ordRejReason));
  if (!r.text.empty()) m.setField(FIX::FIELD::Text, r.text);
  m.setField(FIX::TransactTime());
  send(m, r.client);
}

void FixGateway::onCancelReject(const CancelReject& r) {
  FIX44::OrderCancelReject m;
  m.setField(FIX::FIELD::OrderID, orderIdText(r.orderId));
  m.setField(FIX::FIELD::ClOrdID, r.clOrdId);
  m.setField(FIX::FIELD::OrigClOrdID, r.origClOrdId);
  m.setField(FIX::FIELD::OrdStatus, std::string(1, fixOrdStatus(r.state)));
  m.setField(FIX::FIELD::CxlRejResponseTo, r.replace ? "2" : "1");
  m.setField(FIX::FIELD::CxlRejReason, std::to_string(r.reason));
  if (!r.text.empty()) m.setField(FIX::FIELD::Text, r.text);
  send(m, r.client);
}

ClientId FixGateway::clientFor(const FIX::SessionID& session) {
  const auto [found, added] = clients_.emplace(session, static_cast<ClientId>(sessions_.size()));
  if (added) sessions_.push_back(session);
  return found->second;
}

void FixGateway::send(FIX::Message& message, ClientId client) {
  // If the client is disconnected, QuickFIX keeps the message in its store and
  // the client recovers it by sequence number when it logs back on.
  FIX::Session::sendToTarget(message, sessions_.at(client));
}

} // namespace oems
