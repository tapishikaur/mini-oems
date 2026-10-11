#include "oems/types.hpp"

#include <cstddef>
#include <limits>

namespace oems {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Parses an optionally signed decimal into an integer scaled by 10^decimals.
// Digits beyond `decimals` places are accepted only if they're zeros, so nothing
// is ever silently rounded.
std::optional<std::int64_t> parseScaled(std::string_view text, int decimals, bool allowNegative) {
  bool negative = false;
  if (!text.empty() && text.front() == '-') {
    if (!allowNegative) return std::nullopt;
    negative = true;
    text.remove_prefix(1);
  }

  std::int64_t value = 0;
  bool sawDigit = false;
  bool sawPoint = false;
  int fractionDigits = 0;
  for (const char c : text) {
    if (c == '.') {
      if (sawPoint) return std::nullopt;
      sawPoint = true;
      continue;
    }
    if (c < '0' || c > '9') return std::nullopt;
    sawDigit = true;
    const int digit = c - '0';
    if (sawPoint) {
      if (fractionDigits == decimals) {
        if (digit != 0) return std::nullopt;  // would need rounding
        continue;
      }
      ++fractionDigits;
    }
    if (value > (kInt64Max - digit) / 10) return std::nullopt;  // would overflow
    value = value * 10 + digit;
  }
  if (!sawDigit) return std::nullopt;

  for (; fractionDigits < decimals; ++fractionDigits) {
    if (value > kInt64Max / 10) return std::nullopt;
    value *= 10;
  }
  return negative ? -value : value;
}

} // namespace

std::optional<Price> parsePrice(std::string_view text) {
  const auto raw = parseScaled(text, Price::kDecimals, /*allowNegative=*/true);
  if (!raw) return std::nullopt;
  return Price::fromRaw(*raw);
}

std::string formatPrice(Price price) {
  const std::int64_t raw = price.raw();
  // Work with the magnitude as unsigned so the most negative value formats correctly.
  const std::uint64_t magnitude =
      raw < 0 ? 0 - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
  const auto scale = static_cast<std::uint64_t>(Price::kScale);

  std::string out = raw < 0 ? "-" : "";
  out += std::to_string(magnitude / scale);

  std::uint64_t fraction = magnitude % scale;
  if (fraction != 0) {
    char digits[Price::kDecimals];
    for (int i = Price::kDecimals - 1; i >= 0; --i) {
      digits[i] = static_cast<char>('0' + fraction % 10);
      fraction /= 10;
    }
    std::size_t length = Price::kDecimals;
    while (digits[length - 1] == '0') --length;  // trim trailing zeros
    out += '.';
    out.append(digits, length);
  }
  return out;
}

std::optional<Qty> parseQty(std::string_view text) {
  return parseScaled(text, /*decimals=*/0, /*allowNegative=*/false);
}

} // namespace oems
