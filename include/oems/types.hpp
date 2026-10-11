#pragma once
// Core value types. Prices and quantities are integers: no floating point is
// used anywhere money is stored, compared or added up.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oems {

using OrderId   = std::uint64_t;
using SymbolId  = std::uint32_t;
using AccountId = std::uint32_t;
using Qty       = std::int64_t;  // whole units (shares)

// 128-bit integer for products like price × quantity, which can overflow 64 bits.
// GCC and Clang provide it; __extension__ keeps -Wpedantic quiet about it.
__extension__ typedef __int128 Int128;

enum class Side : std::uint8_t { Buy, Sell };

// A price stored as a whole number of 1/10,000ths (4 decimal places), so 150.25
// is stored as 1'502'500. Exact, cheap to compare, and safe to use as a map key.
class Price {
public:
  static constexpr std::int64_t kScale = 10'000;
  static constexpr int kDecimals = 4;

  constexpr Price() = default;
  static constexpr Price fromRaw(std::int64_t raw) { return Price(raw); }
  // For constants in code and tests, e.g. Price::fromUnits(10) is 10.0000.
  static constexpr Price fromUnits(std::int64_t units) { return Price(units * kScale); }

  constexpr std::int64_t raw() const { return raw_; }
  constexpr auto operator<=>(const Price&) const = default;

private:
  constexpr explicit Price(std::int64_t raw) : raw_(raw) {}
  std::int64_t raw_ = 0;
};

// Parses a FIX decimal such as "150.25", "-0.5" or "100". Never rounds: returns
// nullopt for malformed text, for non-zero digits past the 4th decimal place,
// and for values that don't fit.
std::optional<Price> parsePrice(std::string_view text);

// Formats with as few decimals as needed: 1'502'500 -> "150.25", 1'500'000 -> "150".
std::string formatPrice(Price price);

// Parses a FIX quantity as whole units: "100" and "100.00" are accepted,
// "100.5" and "-5" are not.
std::optional<Qty> parseQty(std::string_view text);

} // namespace oems
