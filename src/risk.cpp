#include "oems/risk.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace oems {
namespace {

constexpr Int128 kBpsPerUnit = 10'000; // basis points in 100%

Qty absQty(Qty q) { return q < 0 ? -q : q; }

} // namespace

RiskEngine::RiskEngine(std::vector<SymbolLimits> symbols, std::vector<AccountLimits> accounts)
    : symbols_(std::move(symbols)),
      accounts_(std::move(accounts)),
      exposures_(symbols_.size() * accounts_.size()),
      gross_(accounts_.size(), 0) {
  for (const SymbolLimits& s : symbols_) {
    if (s.tickSize.raw() <= 0 || s.referencePrice.raw() <= 0 || s.priceBandBps < 0 || s.maxOrderQty < 0) {
      throw std::invalid_argument("symbol limits need a positive tick size and reference price");
    }
  }
}

RiskReject RiskEngine::checkNew(const RiskOrder& order) const {
  const RiskReject reject = checkOrder(order.account, order.symbol, order.price, order.qty);
  if (reject != RiskReject::None) return reject;
  return checkExposure(order.account, order.symbol, order.side, order.qty);
}

RiskReject RiskEngine::checkReplace(const RiskReplace& replace) const {
  const RiskReject reject = checkOrder(replace.account, replace.symbol, replace.newPrice, replace.newOrderQty);
  if (reject != RiskReject::None) return reject;
  if (replace.newOrderQty < replace.cumQty) return RiskReject::InvalidQuantity;
  const Qty newOpen = replace.newOrderQty - replace.cumQty;
  return checkExposure(replace.account, replace.symbol, replace.side, newOpen - replace.openQty);
}

// The checks that look only at the order itself, cheapest first.
RiskReject RiskEngine::checkOrder(AccountId account, SymbolId symbol, Price price, Qty qty) const {
  if (account >= accounts_.size()) return RiskReject::UnknownAccount;
  if (symbol >= symbols_.size()) return RiskReject::UnknownSymbol;
  const SymbolLimits& limits = symbols_[symbol];
  if (!limits.tradable) return RiskReject::SymbolNotTradable;
  if (qty <= 0) return RiskReject::InvalidQuantity;
  if (qty > limits.maxOrderQty) return RiskReject::QuantityAboveMax;
  if (price.raw() <= 0) return RiskReject::InvalidPrice;
  if (price.raw() % limits.tickSize.raw() != 0) return RiskReject::OffTick;

  // |price - reference| / reference <= band, without division or rounding.
  const std::int64_t reference = limits.referencePrice.raw();
  const std::int64_t distance = price.raw() > reference ? price.raw() - reference : reference - price.raw();
  if (Int128{distance} * kBpsPerUnit > Int128{limits.priceBandBps} * reference) {
    return RiskReject::OutsidePriceBand;
  }

  // 128-bit, so a huge price × quantity can't wrap around and slip under the limit.
  if (Int128{price.raw()} * qty > accounts_[account].maxOrderNotional) return RiskReject::NotionalAboveMax;
  return RiskReject::None;
}

// The checks that depend on what the account already holds and has open.
RiskReject RiskEngine::checkExposure(AccountId account, SymbolId symbol, Side side, Qty openDelta) const {
  const Exposure& now = exposures_[index(account, symbol)];
  Exposure after = now;
  (side == Side::Buy ? after.openBuy : after.openSell) += openDelta;

  const Qty worstNow = worstCase(now);
  const Qty worstAfter = worstCase(after);
  if (worstAfter <= worstNow) return RiskReject::None; // adds no risk: always allowed

  const AccountLimits& limits = accounts_[account];
  if (worstAfter > limits.maxPositionPerSymbol) return RiskReject::PositionLimit;
  const Int128 grossAfter =
      gross_[account] + Int128{worstAfter - worstNow} * symbols_[symbol].referencePrice.raw();
  if (grossAfter > limits.maxGrossExposure) return RiskReject::GrossExposureLimit;
  return RiskReject::None;
}

void RiskEngine::onOpenChange(AccountId account, SymbolId symbol, Side side, Qty delta) {
  update(account, symbol, [&](Exposure& e) { (side == Side::Buy ? e.openBuy : e.openSell) += delta; });
}

void RiskEngine::onFill(AccountId account, SymbolId symbol, Side side, Qty qty) {
  update(account, symbol, [&](Exposure& e) {
    if (side == Side::Buy) {
      e.openBuy -= qty;
      e.position += qty;
    } else {
      e.openSell -= qty;
      e.position -= qty;
    }
  });
}

const RiskEngine::Exposure& RiskEngine::exposure(AccountId account, SymbolId symbol) const {
  return exposures_[index(account, symbol)];
}

Qty RiskEngine::worstCase(const Exposure& e) {
  return std::max(absQty(e.position + e.openBuy), absQty(e.position - e.openSell));
}

// Applies a change to one exposure and moves the account's gross exposure by
// the same amount, so the gross never has to be re-summed over all symbols.
template <typename Change> void RiskEngine::update(AccountId account, SymbolId symbol, Change change) {
  assert(account < accounts_.size() && symbol < symbols_.size());
  Exposure& e = exposures_[index(account, symbol)];
  const Qty before = worstCase(e);
  change(e);
  assert(e.openBuy >= 0 && e.openSell >= 0);
  gross_[account] += Int128{worstCase(e) - before} * symbols_[symbol].referencePrice.raw();
}

std::string_view toString(RiskReject reject) {
  switch (reject) {
  case RiskReject::None: return "None";
  case RiskReject::UnknownAccount: return "UnknownAccount";
  case RiskReject::UnknownSymbol: return "UnknownSymbol";
  case RiskReject::SymbolNotTradable: return "SymbolNotTradable";
  case RiskReject::InvalidQuantity: return "InvalidQuantity";
  case RiskReject::QuantityAboveMax: return "QuantityAboveMax";
  case RiskReject::InvalidPrice: return "InvalidPrice";
  case RiskReject::OffTick: return "OffTick";
  case RiskReject::OutsidePriceBand: return "OutsidePriceBand";
  case RiskReject::NotionalAboveMax: return "NotionalAboveMax";
  case RiskReject::PositionLimit: return "PositionLimit";
  case RiskReject::GrossExposureLimit: return "GrossExposureLimit";
  }
  return "?";
}

} // namespace oems
