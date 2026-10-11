#include "oems/order_manager.hpp"

#include <cassert>
#include <utility>
#include <variant>

namespace oems {
namespace {

// What risk counts as open for an order: nothing until the venue has accepted it.
Qty riskOpen(const Order& order) { return order.state() == OrdState::PendingNew ? 0 : order.leavesQty(); }

} // namespace

char fixOrdStatus(OrdState state) {
  switch (state) {
  case OrdState::PendingNew: return 'A';
  case OrdState::New: return '0';
  case OrdState::PartiallyFilled: return '1';
  case OrdState::Filled: return '2';
  case OrdState::PendingCancel: return '6';
  case OrdState::Canceled: return '4';
  case OrdState::PendingReplace: return 'E';
  case OrdState::Rejected: return '8';
  }
  return '8';
}

int ordRejReason(RiskReject reject) {
  using namespace ord_rej_reason;
  switch (reject) {
  case RiskReject::UnknownAccount: return UnknownAccount;
  case RiskReject::UnknownSymbol: return UnknownSymbol;
  case RiskReject::SymbolNotTradable: return ExchangeClosed;
  case RiskReject::InvalidQuantity: return IncorrectQuantity;
  case RiskReject::QuantityAboveMax:
  case RiskReject::NotionalAboveMax:
  case RiskReject::PositionLimit:
  case RiskReject::GrossExposureLimit: return OrderExceedsLimit;
  case RiskReject::None:
  case RiskReject::InvalidPrice:
  case RiskReject::OffTick:
  case RiskReject::OutsidePriceBand: break;
  }
  return Other;
}

void OrderManager::onNewOrder(const ClientNewOrder& request) {
  assert(router_ && sink_ && "call connect() first");
  ClOrdIdMap& ids = clOrdIds_[request.client];

  if (const auto seen = ids.find(request.clOrdId); seen != ids.end()) {
    // A possible resend (say, after a reconnect) of a NewOrderSingle we already
    // have gets that order's current status and changes nothing. Any other reuse
    // of a ClOrdID, including one a cancel or replace used, is a client bug and is
    // rejected.
    if (request.possResend && seen->second != 0) {
      const Record& original = orders_.at(seen->second);
      if (original.firstClOrdId == request.clOrdId) {
        Record status = original;
        status.clOrdId = request.clOrdId; // answer under the ClOrdID the client asked about
        report(status, ExecType::OrderStatus);
        return;
      }
    }
    ExecutionReport duplicate;
    duplicate.client = request.client;
    duplicate.execId = nextExecId_++;
    duplicate.execType = ExecType::Rejected;
    duplicate.state = OrdState::Rejected;
    duplicate.clOrdId = request.clOrdId;
    duplicate.account = request.account;
    duplicate.symbol = request.symbol;
    duplicate.side = request.side;
    duplicate.tif = request.tif;
    duplicate.price = request.price;
    duplicate.orderQty = request.qty;
    duplicate.ordRejReason = ord_rej_reason::DuplicateOrder;
    duplicate.text = "duplicate ClOrdID";
    sink_->onExecutionReport(duplicate);
    return;
  }

  // Every new ClOrdID gets an order, even one about to be rejected: the client
  // gets an OrderID, and the ClOrdID can't be reused.
  const OrderId id = nextOrderId_++;
  ids.emplace(request.clOrdId, id);
  Record& record =
      orders_
          .emplace(id, Record{Order(id, request.symbol, request.account, request.side, request.price, request.qty),
                              request.client, request.tif, request.clOrdId, request.clOrdId, {}})
          .first->second;

  if (request.invalid) {
    reject(record, request.invalid->reason, request.invalid->text);
    return;
  }
  const RiskReject risk =
      risk_.checkNew({request.account, request.symbol, request.side, request.price, request.qty});
  if (risk != RiskReject::None) {
    reject(record, ordRejReason(risk), std::string(toString(risk)));
    return;
  }
  // The venue answers synchronously: onVenueEvent() runs before submit() returns.
  router_->submit({id, request.symbol, request.side, request.price, request.qty, request.tif});
}

void OrderManager::onCancel(const ClientCancel& request) {
  Record* record = admitRequest(request.client, request.clOrdId, request.origClOrdId, /*replace=*/false);
  if (!record) return;
  [[maybe_unused]] const ApplyResult result = record->order.apply(event::CancelRequest{});
  assert(result == ApplyResult::Ok); // admitRequest() only lets live orders through
  record->requestClOrdId = request.clOrdId;
  router_->cancel(record->order.id());
}

void OrderManager::onReplace(const ClientReplace& request) {
  Record* record = admitRequest(request.client, request.clOrdId, request.origClOrdId, /*replace=*/true);
  if (!record) return;
  const Order& order = record->order;

  if (request.invalid) {
    cancelReject(*record, request.clOrdId, true, request.invalid->reason, request.invalid->text);
    return;
  }
  if (request.side != order.side() || request.symbol != order.symbol()) {
    cancelReject(*record, request.clOrdId, true, cxl_rej_reason::Other, "side and symbol can't be changed");
    return;
  }
  const RiskReject risk = risk_.checkReplace({order.account(), order.symbol(), order.side(), order.leavesQty(),
                                              order.cumQty(), request.price, request.qty});
  if (risk != RiskReject::None) {
    cancelReject(*record, request.clOrdId, true, cxl_rej_reason::Other, std::string(toString(risk)));
    return;
  }

  [[maybe_unused]] const ApplyResult result = record->order.apply(event::ReplaceRequest{});
  assert(result == ApplyResult::Ok);
  record->requestClOrdId = request.clOrdId;
  router_->replace(order.id(), request.qty, request.price);
}

void OrderManager::onVenueEvent(OrderId id, const OrderEvent& event) {
  const auto found = orders_.find(id);
  if (found == orders_.end()) {
    ++venueAnomalies_;
    return;
  }
  Record& record = found->second;
  Order& order = record.order;

  const Qty openBefore = riskOpen(order);
  if (order.apply(event) != ApplyResult::Ok) {
    ++venueAnomalies_; // the venue and the state machine disagree; tests show this never happens
    return;
  }

  // Keep risk exposure in step with the order.
  if (const auto* fill = std::get_if<event::Fill>(&event)) {
    risk_.onFill(order.account(), order.symbol(), order.side(), fill->qty);
    report(record, ExecType::Trade, {}, fill->qty, fill->price);
    return;
  }
  if (const Qty delta = riskOpen(order) - openBefore; delta != 0) {
    risk_.onOpenChange(order.account(), order.symbol(), order.side(), delta);
  }

  if (std::holds_alternative<event::Accept>(event)) {
    report(record, ExecType::New);
  } else if (std::holds_alternative<event::Reject>(event)) {
    report(record, ExecType::Rejected, {}, 0, {}, ord_rej_reason::Other, "rejected by venue");
  } else if (std::holds_alternative<event::VenueCancel>(event)) {
    report(record, ExecType::Canceled, {}, 0, {}, -1, "unfilled quantity cancelled (IOC)");
  } else if (std::holds_alternative<event::CancelDone>(event) ||
             std::holds_alternative<event::ReplaceDone>(event)) {
    // The order now goes by the request's ClOrdID; the report names the old one too.
    std::string previous = std::exchange(record.clOrdId, record.requestClOrdId);
    const bool cancelled = std::holds_alternative<event::CancelDone>(event);
    report(record, cancelled ? ExecType::Canceled : ExecType::Replaced, std::move(previous));
  } else if (std::holds_alternative<event::CancelReject>(event)) {
    cancelReject(record, record.requestClOrdId, false, cxl_rej_reason::TooLateToCancel, "order is no longer open");
  } else if (std::holds_alternative<event::ReplaceReject>(event)) {
    cancelReject(record, record.requestClOrdId, true, cxl_rej_reason::TooLateToCancel, "order is no longer open");
  }
}

const Order* OrderManager::order(OrderId id) const {
  const auto found = orders_.find(id);
  return found == orders_.end() ? nullptr : &found->second.order;
}

OrderId OrderManager::orderIdFor(ClientId client, std::string_view clOrdId) const {
  const auto ids = clOrdIds_.find(client);
  if (ids == clOrdIds_.end()) return 0;
  const auto found = ids->second.find(clOrdId);
  return found == ids->second.end() ? 0 : found->second;
}

OrderManager::Record* OrderManager::admitRequest(ClientId client, const std::string& clOrdId,
                                                 const std::string& origClOrdId, bool replace) {
  assert(router_ && sink_ && "call connect() first");
  ClOrdIdMap& ids = clOrdIds_[client];
  const auto orig = ids.find(origClOrdId);
  const OrderId target = orig == ids.end() ? 0 : orig->second;
  Record* record = target == 0 ? nullptr : &orders_.at(target);

  const auto refuse = [&](int reason, std::string text) -> Record* {
    CancelReject reject;
    reject.client = client;
    reject.orderId = target;
    reject.clOrdId = clOrdId;
    reject.origClOrdId = origClOrdId;
    reject.state = record ? record->order.state() : OrdState::Rejected;
    reject.replace = replace;
    reject.reason = reason;
    reject.text = std::move(text);
    sink_->onCancelReject(reject);
    return nullptr;
  };

  if (ids.contains(clOrdId)) return refuse(cxl_rej_reason::DuplicateClOrdId, "duplicate ClOrdID");
  ids.emplace(clOrdId, target); // used now, whatever happens next
  if (!record) return refuse(cxl_rej_reason::UnknownOrder, "unknown order");

  const OrdState state = record->order.state();
  if (isTerminal(state)) return refuse(cxl_rej_reason::TooLateToCancel, "order is " + std::string(toString(state)));
  if (state != OrdState::New && state != OrdState::PartiallyFilled) {
    return refuse(cxl_rej_reason::AlreadyPending, "order has a request pending");
  }
  return record;
}

void OrderManager::reject(Record& record, int reason, std::string text) {
  [[maybe_unused]] const ApplyResult result = record.order.apply(event::Reject{});
  assert(result == ApplyResult::Ok);
  report(record, ExecType::Rejected, {}, 0, {}, reason, std::move(text));
}

void OrderManager::report(const Record& record, ExecType type, std::string origClOrdId, Qty lastQty,
                          Price lastPx, int rejectReason, std::string text) {
  const Order& order = record.order;
  ExecutionReport r;
  r.client = record.client;
  r.orderId = order.id();
  r.execId = nextExecId_++;
  r.execType = type;
  r.state = order.state();
  r.clOrdId = record.clOrdId;
  r.origClOrdId = std::move(origClOrdId);
  r.account = order.account();
  r.symbol = order.symbol();
  r.side = order.side();
  r.tif = record.tif;
  r.price = order.price();
  r.orderQty = order.orderQty();
  r.lastQty = lastQty;
  r.lastPx = lastPx;
  r.cumQty = order.cumQty();
  r.leavesQty = order.leavesQty();
  r.avgPx = order.avgPx();
  r.ordRejReason = rejectReason;
  r.text = std::move(text);
  sink_->onExecutionReport(r);
}

void OrderManager::cancelReject(const Record& record, const std::string& requestClOrdId, bool replace,
                                int reason, std::string text) {
  CancelReject reject;
  reject.client = record.client;
  reject.orderId = record.order.id();
  reject.clOrdId = requestClOrdId;
  reject.origClOrdId = record.clOrdId;
  reject.state = record.order.state();
  reject.replace = replace;
  reject.reason = reason;
  reject.text = std::move(text);
  sink_->onCancelReject(reject);
}

} // namespace oems
