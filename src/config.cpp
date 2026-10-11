#include "oems/config.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oems {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Splits one data line into exactly `count` fields, or throws.
std::vector<std::string_view> fields(std::string_view line, std::size_t count, int lineNumber) {
  std::vector<std::string_view> out;
  while (true) {
    const std::size_t comma = line.find(',');
    out.push_back(trim(line.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    line.remove_prefix(comma + 1);
  }
  if (out.size() != count) {
    throw std::runtime_error("line " + std::to_string(lineNumber) + ": expected " + std::to_string(count) +
                             " fields, got " + std::to_string(out.size()));
  }
  return out;
}

[[noreturn]] void bad(int lineNumber, std::string_view what, std::string_view value) {
  throw std::runtime_error("line " + std::to_string(lineNumber) + ": bad " + std::string(what) + " '" +
                           std::string(value) + "'");
}

std::int64_t integer(std::string_view text, int lineNumber, std::string_view what) {
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0) bad(lineNumber, what, text);
  return value;
}

Price price(std::string_view text, int lineNumber, std::string_view what) {
  const std::optional<Price> p = parsePrice(text);
  if (!p || p->raw() <= 0) bad(lineNumber, what, text);
  return *p;
}

// Calls handle(fields, lineNumber) for every data line.
template <typename Handle> void forEachLine(std::istream& in, std::size_t fieldCount, Handle handle) {
  std::string line;
  for (int lineNumber = 1; std::getline(in, line); ++lineNumber) {
    const std::string_view content = trim(line);
    if (content.empty() || content.front() == '#') continue;
    handle(fields(content, fieldCount, lineNumber), lineNumber);
  }
}

} // namespace

InstrumentConfig loadInstruments(std::istream& in) {
  InstrumentConfig config;
  forEachLine(in, 6, [&](const std::vector<std::string_view>& f, int n) {
    SymbolLimits limits;
    limits.tickSize = price(f[1], n, "tick size");
    limits.referencePrice = price(f[2], n, "reference price");
    limits.priceBandBps = integer(f[3], n, "price band");
    limits.maxOrderQty = integer(f[4], n, "max order quantity");
    if (f[5] != "Y" && f[5] != "N") bad(n, "tradable flag (Y or N)", f[5]);
    limits.tradable = f[5] == "Y";
    if (limits.referencePrice.raw() % limits.tickSize.raw() != 0) bad(n, "reference price (not on tick)", f[2]);
    try {
      config.symbols.add(std::string(f[0]));
    } catch (const std::invalid_argument&) {
      bad(n, "symbol (listed twice)", f[0]);
    }
    config.limits.push_back(limits);
  });
  return config;
}

AccountConfig loadAccounts(std::istream& in) {
  AccountConfig config;
  forEachLine(in, 4, [&](const std::vector<std::string_view>& f, int n) {
    AccountLimits limits;
    limits.maxOrderNotional = notional(integer(f[1], n, "max order notional"));
    limits.maxPositionPerSymbol = integer(f[2], n, "max position per symbol");
    limits.maxGrossExposure = notional(integer(f[3], n, "max gross exposure"));
    try {
      config.accounts.add(std::string(f[0]));
    } catch (const std::invalid_argument&) {
      bad(n, "account (listed twice)", f[0]);
    }
    config.limits.push_back(limits);
  });
  return config;
}

} // namespace oems
