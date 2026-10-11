#pragma once
// Pre-trade risk checks. Every new order and every replace is checked before it
// reaches the venue, cheapest check first; the first failure is the reject
// reason. Cancels are never checked: reducing risk must always be possible.
//
// Speed: symbols and accounts are small integer IDs, so every limit and
// exposure sits in a flat array. A check is a handful of array reads and integer
// comparisons: no hashing, no memory allocation, no locks.
//
// Exposure is as of the last venue event. That's exact here because the
// in-process venue answers synchronously, so no order is ever in flight between
// its check and its acceptance. With an asynchronous venue you would reserve an
// order's quantity when it passes the check and release it if the venue rejects.

#include "oems/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace oems {

// A money amount in the same units as Price::raw() × quantity, e.g.
// notional(1'000'000) is one million currency units.
constexpr Int128 notional(std::int64_t currencyUnits) { return Int128{currencyUnits} * Price::kScale; }

struct SymbolLimits {
  bool tradable = true;
  Qty maxOrderQty = 0;             // largest single order
  Price tickSize;                  // prices must be a whole number of ticks
  Price referencePrice;            // centre of the price band; also used to value exposure
  std::int64_t priceBandBps = 0;   // band half-width in basis points (100 bps = 1%)
};

struct AccountLimits {
  Int128 maxOrderNotional = 0;     // price × quantity of a single order
  Qty maxPositionPerSymbol = 0;    // worst-case |position| in any one symbol
  Int128 maxGrossExposure = 0;     // worst-case |position| × reference price, summed over symbols
};

enum class RiskReject : std::uint8_t {
  None, // passed every check
  UnknownAccount,
  UnknownSymbol,
  SymbolNotTradable,
  InvalidQuantity, // zero, negative, or a replace below what has already filled
  QuantityAboveMax,
  InvalidPrice,    // zero or negative
  OffTick,
  OutsidePriceBand,
  NotionalAboveMax,
  PositionLimit,
  GrossExposureLimit,
};
std::string_view toString(RiskReject reject);

struct RiskOrder {
  AccountId account;
  SymbolId symbol;
  Side side;
  Price price;
  Qty qty;
};

// A request to change a live order to newPrice / newOrderQty. Quantities are
// FIX-style: newOrderQty is the new total, including cumQty already filled.
struct RiskReplace {
  AccountId account;
  SymbolId symbol;
  Side side;
  Qty openQty; // open now
  Qty cumQty;  // filled so far
  Price newPrice;
  Qty newOrderQty;
};

class RiskEngine {
public:
  // Symbol and account IDs are indexes into these vectors. Throws
  // std::invalid_argument for limits that make no sense, such as a zero tick size.
  RiskEngine(std::vector<SymbolLimits> symbols, std::vector<AccountLimits> accounts);

  // Pure checks: they change nothing.
  RiskReject checkNew(const RiskOrder& order) const;
  RiskReject checkReplace(const RiskReplace& replace) const;

  // Keeping exposure current, called by the order manager on venue events.
  // Open quantity changed: +qty when an order is accepted, -qty when it's
  // cancelled, the difference when it's replaced.
  void onOpenChange(AccountId account, SymbolId symbol, Side side, Qty delta);
  // A fill: moves qty from open to position.
  void onFill(AccountId account, SymbolId symbol, Side side, Qty qty);

  struct Exposure {
    Qty position = 0; // net: bought minus sold
    Qty openBuy = 0;
    Qty openSell = 0;
  };
  const Exposure& exposure(AccountId account, SymbolId symbol) const;
  Int128 grossExposure(AccountId account) const { return gross_[account]; }

  // The largest |position| this exposure could reach if every open order on one
  // side filled.
  static Qty worstCase(const Exposure& e);

private:
  RiskReject checkOrder(AccountId account, SymbolId symbol, Price price, Qty qty) const;
  RiskReject checkExposure(AccountId account, SymbolId symbol, Side side, Qty openDelta) const;
  std::size_t index(AccountId account, SymbolId symbol) const {
    return static_cast<std::size_t>(account) * symbols_.size() + symbol;
  }
  template <typename Change> void update(AccountId account, SymbolId symbol, Change change);

  std::vector<SymbolLimits> symbols_;
  std::vector<AccountLimits> accounts_;
  std::vector<Exposure> exposures_; // accounts × symbols, row by row
  std::vector<Int128> gross_;       // per account
};

} // namespace oems
