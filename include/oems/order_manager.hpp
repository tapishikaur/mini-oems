#pragma once
// The order manager: the FIX-free core of the gateway. It takes client requests
// (already parsed from FIX) and:
//   - checks ClOrdIDs for duplicates, telling a resend apart from a client bug;
//   - runs pre-trade risk;
//   - sends accepted requests to the venue;
//   - applies every venue event to the order's state machine;
//   - keeps risk exposure in step with every order.
// Everything it does is reported as an ExecutionReport or a CancelReject, ready
// for the gateway to write out as FIX 35=8 / 35=9.
//
// Single-threaded: every call, including venue callbacks, must come from one thread.

#include "oems/order.hpp"
#include "oems/risk.hpp"
#include "oems/venue.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oems {

using ClientId = std::uint32_t; // one per FIX session

// FIX 4.4 ExecType (tag 150) values this system sends.
enum class ExecType : char {
  New = '0',
  Canceled = '4',
  Replaced = '5',
  Rejected = '8',
  Trade = 'F',
  OrderStatus = 'I', // answers a resend of an order we already have
};

char fixOrdStatus(OrdState state); // FIX 4.4 OrdStatus (tag 39)

// FIX 4.4 reject codes, as listed in QuickFIX's FIX44.xml.
namespace ord_rej_reason { // OrdRejReason, tag 103
inline constexpr int UnknownSymbol = 1;
inline constexpr int ExchangeClosed = 2;
inline constexpr int OrderExceedsLimit = 3;
inline constexpr int DuplicateOrder = 6;
inline constexpr int UnsupportedOrderCharacteristic = 11;
inline constexpr int IncorrectQuantity = 13;
inline constexpr int UnknownAccount = 15;
inline constexpr int Other = 99;
} // namespace ord_rej_reason

namespace cxl_rej_reason { // CxlRejReason, tag 102
inline constexpr int TooLateToCancel = 0;
inline constexpr int UnknownOrder = 1;
inline constexpr int AlreadyPending = 3;
inline constexpr int DuplicateClOrdId = 6;
inline constexpr int Other = 99;
} // namespace cxl_rej_reason

int ordRejReason(RiskReject reject);

// A request the gateway couldn't turn into a valid order: unknown symbol name,
// unsupported order type, malformed price text, and so on.
struct InvalidRequest {
  int reason; // OrdRejReason for new orders; CxlRejReason for replaces
  std::string text;
};

struct ClientNewOrder {
  ClientId client = 0;
  std::string clOrdId;
  AccountId account = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Price price;
  Qty qty = 0;
  TimeInForce tif = TimeInForce::Day;
  // FIX PossResend (97=Y) or PossDupFlag (43=Y): this may repeat a NewOrderSingle
  // we already have. If it does, it gets a status report instead of a second order.
  bool possResend = false;
  std::optional<InvalidRequest> invalid;
};

struct ClientCancel {
  ClientId client = 0;
  std::string clOrdId;     // this request's own ID
  std::string origClOrdId; // the order's current ClOrdID
};

struct ClientReplace {
  ClientId client = 0;
  std::string clOrdId;
  std::string origClOrdId;
  SymbolId symbol = 0; // must match the order
  Side side = Side::Buy; // must match the order
  Price price;
  Qty qty = 0; // new total quantity, including anything already filled
  std::optional<InvalidRequest> invalid;
};

struct ExecutionReport {
  ClientId client = 0;
  OrderId orderId = 0; // 0 when no order exists: a rejected duplicate ClOrdID
  std::uint64_t execId = 0;
  ExecType execType = ExecType::New;
  OrdState state = OrdState::New;
  std::string clOrdId;
  std::string origClOrdId; // set on Canceled / Replaced reports that answer a request
  AccountId account = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  TimeInForce tif = TimeInForce::Day;
  Price price;
  Qty orderQty = 0;
  Qty lastQty = 0; // Trade reports only
  Price lastPx;    // Trade reports only
  Qty cumQty = 0;
  Qty leavesQty = 0;
  Price avgPx;
  int ordRejReason = -1; // Rejected reports only
  std::string text;
};

struct CancelReject {
  ClientId client = 0;
  OrderId orderId = 0; // 0 if the order is unknown
  std::string clOrdId;
  std::string origClOrdId;
  OrdState state = OrdState::Rejected; // the order's current state; Rejected if unknown
  bool replace = false; // answers a cancel/replace (35=G) rather than a cancel (35=F)
  int reason = cxl_rej_reason::Other;
  std::string text;
};

class ReportSink {
public:
  virtual ~ReportSink() = default;
  virtual void onExecutionReport(const ExecutionReport& report) = 0;
  virtual void onCancelReject(const CancelReject& reject) = 0;
};

class OrderManager final : public VenueListener {
public:
  explicit OrderManager(RiskEngine& risk) : risk_(risk) {}

  // Call once, before any request. It's a separate step because the venue and
  // the sink usually need a reference to this object first.
  void connect(IOrderRouter& router, ReportSink& sink) {
    router_ = &router;
    sink_ = &sink;
  }

  void onNewOrder(const ClientNewOrder& request);
  void onCancel(const ClientCancel& request);
  void onReplace(const ClientReplace& request);
  void onVenueEvent(OrderId id, const OrderEvent& event) override;

  // For tests and diagnostics.
  const Order* order(OrderId id) const;
  OrderId orderIdFor(ClientId client, std::string_view clOrdId) const; // 0 if none
  std::size_t orderCount() const { return orders_.size(); }
  std::size_t venueAnomalies() const { return venueAnomalies_; } // venue events the state machine refused

private:
  struct Record {
    Order order;
    ClientId client;
    TimeInForce tif;
    std::string firstClOrdId;   // the NewOrderSingle's ClOrdID: what a resend would repeat
    std::string clOrdId;        // current; changes when a cancel or replace succeeds
    std::string requestClOrdId; // ClOrdID of the cancel/replace in progress
  };

  // ClOrdID -> order, per client. Every ClOrdID a client has used is here, so any
  // reuse is caught; 0 means "used, but by a request that had no order".
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  using ClOrdIdMap = std::unordered_map<std::string, OrderId, StringHash, std::equal_to<>>;

  // Checks a cancel/replace's ClOrdIDs and the order's state. Returns the order to
  // act on, or sends a CancelReject and returns nullptr.
  Record* admitRequest(ClientId client, const std::string& clOrdId, const std::string& origClOrdId,
                       bool replace);
  void reject(Record& record, int reason, std::string text);
  void report(const Record& record, ExecType type, std::string origClOrdId = {}, Qty lastQty = 0,
              Price lastPx = {}, int rejectReason = -1, std::string text = {});
  void cancelReject(const Record& record, const std::string& requestClOrdId, bool replace, int reason,
                    std::string text);

  RiskEngine& risk_;
  IOrderRouter* router_ = nullptr;
  ReportSink* sink_ = nullptr;
  std::unordered_map<OrderId, Record> orders_;
  std::unordered_map<ClientId, ClOrdIdMap> clOrdIds_;
  OrderId nextOrderId_ = 1;
  std::uint64_t nextExecId_ = 1;
  std::size_t venueAnomalies_ = 0;
};

} // namespace oems
