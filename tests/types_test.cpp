#include "oems/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <random>

namespace oems {
namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

struct DecimalCase {
  const char* text;
  std::optional<std::int64_t> expected; // nullopt: must be rejected
};

void PrintTo(const DecimalCase& c, std::ostream* os) { *os << '"' << c.text << '"'; }

class ParsePriceTest : public ::testing::TestWithParam<DecimalCase> {};

TEST_P(ParsePriceTest, ParsesExactlyOrRejects) {
  const DecimalCase& c = GetParam();
  const std::optional<Price> parsed = parsePrice(c.text);
  if (c.expected) {
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->raw(), *c.expected);
  } else {
    EXPECT_FALSE(parsed.has_value()) << "parsed as raw " << parsed->raw();
  }
}

INSTANTIATE_TEST_SUITE_P(Valid, ParsePriceTest,
                         ::testing::Values(DecimalCase{"150.25", 1'502'500},
                                           DecimalCase{"150", 1'500'000},
                                           DecimalCase{"0.0001", 1},
                                           DecimalCase{"0", 0},
                                           DecimalCase{"-1.5", -15'000},
                                           DecimalCase{"1.", 10'000},
                                           DecimalCase{".5", 5'000},
                                           DecimalCase{"007.10", 71'000},
                                           DecimalCase{"1.23450000", 12'345}, // extra zeros are exact
                                           DecimalCase{"922337203685477.5807", kMax}));

INSTANTIATE_TEST_SUITE_P(Invalid, ParsePriceTest,
                         ::testing::Values(DecimalCase{"", std::nullopt},
                                           DecimalCase{".", std::nullopt},
                                           DecimalCase{"-", std::nullopt},
                                           DecimalCase{"1.23456", std::nullopt}, // would need rounding
                                           DecimalCase{"1.2.3", std::nullopt},
                                           DecimalCase{"1e3", std::nullopt},
                                           DecimalCase{" 1", std::nullopt},
                                           DecimalCase{"1 ", std::nullopt},
                                           DecimalCase{"+1", std::nullopt},
                                           DecimalCase{"1,5", std::nullopt},
                                           DecimalCase{"--1", std::nullopt},
                                           DecimalCase{"922337203685477.5808", std::nullopt}, // overflow
                                           DecimalCase{"99999999999999999999", std::nullopt}));

class ParseQtyTest : public ::testing::TestWithParam<DecimalCase> {};

TEST_P(ParseQtyTest, AcceptsOnlyWholeNonNegativeUnits) {
  const DecimalCase& c = GetParam();
  const std::optional<Qty> parsed = parseQty(c.text);
  if (c.expected) {
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, *c.expected);
  } else {
    EXPECT_FALSE(parsed.has_value()) << "parsed as " << *parsed;
  }
}

INSTANTIATE_TEST_SUITE_P(All, ParseQtyTest,
                         ::testing::Values(DecimalCase{"100", 100},
                                           DecimalCase{"100.00", 100},
                                           DecimalCase{"0", 0},
                                           DecimalCase{"100.5", std::nullopt},
                                           DecimalCase{"-5", std::nullopt},
                                           DecimalCase{"", std::nullopt},
                                           DecimalCase{"1e2", std::nullopt}));

TEST(FormatPrice, UsesAsFewDecimalsAsNeeded) {
  EXPECT_EQ(formatPrice(Price::fromRaw(1'502'500)), "150.25");
  EXPECT_EQ(formatPrice(Price::fromRaw(1'500'000)), "150");
  EXPECT_EQ(formatPrice(Price::fromRaw(1)), "0.0001");
  EXPECT_EQ(formatPrice(Price::fromRaw(0)), "0");
  EXPECT_EQ(formatPrice(Price::fromRaw(-15'000)), "-1.5");
  EXPECT_EQ(formatPrice(Price::fromRaw(kMax)), "922337203685477.5807");
  EXPECT_EQ(formatPrice(Price::fromRaw(std::numeric_limits<std::int64_t>::min())),
            "-922337203685477.5808");
}

TEST(FormatPrice, RoundTripsThroughParse) {
  std::mt19937_64 rng(20261011); // fixed seed: the same values every run
  std::uniform_int_distribution<int> digits(1, 18);
  for (int i = 0; i < 100'000; ++i) {
    // Spread values across magnitudes, not just near the maximum.
    std::int64_t limit = 1;
    for (int d = digits(rng); d > 0; --d) limit *= 10;
    std::uniform_int_distribution<std::int64_t> value(-limit, limit);
    const Price price = Price::fromRaw(value(rng));

    const std::optional<Price> back = parsePrice(formatPrice(price));
    ASSERT_TRUE(back.has_value()) << formatPrice(price);
    ASSERT_EQ(back->raw(), price.raw()) << formatPrice(price);
  }
}

TEST(Price, ComparesByValue) {
  EXPECT_LT(Price::fromRaw(1), Price::fromRaw(2));
  EXPECT_EQ(Price::fromUnits(10), Price::fromRaw(100'000));
  EXPECT_GT(Price::fromRaw(0), Price::fromRaw(-1));
}

} // namespace
} // namespace oems
