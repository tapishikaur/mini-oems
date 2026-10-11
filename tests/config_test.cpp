#include "oems/config.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace oems {
namespace {

InstrumentConfig instruments(const std::string& text) {
  std::istringstream in(text);
  return loadInstruments(in);
}
AccountConfig accounts(const std::string& text) {
  std::istringstream in(text);
  return loadAccounts(in);
}

// The error message for a bad file, or "" if it loaded.
template <typename Load> std::string errorFrom(Load load) {
  try {
    load();
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return "";
}

TEST(Config, LoadsInstrumentsSkippingCommentsAndBlankLines) {
  const InstrumentConfig c = instruments("# header\n\nAAPL, 0.01, 150.00, 500, 10000, Y\r\n  MSFT,0.05,400,250,20,N\n");
  ASSERT_EQ(c.symbols.size(), 2U);
  EXPECT_EQ(c.symbols.find("AAPL"), 0U);
  EXPECT_EQ(c.symbols.find("MSFT"), 1U);
  EXPECT_EQ(c.limits[0].tickSize, Price::fromRaw(100));
  EXPECT_EQ(c.limits[0].referencePrice, Price::fromUnits(150));
  EXPECT_EQ(c.limits[0].priceBandBps, 500);
  EXPECT_EQ(c.limits[0].maxOrderQty, 10'000);
  EXPECT_TRUE(c.limits[0].tradable);
  EXPECT_FALSE(c.limits[1].tradable);
}

TEST(Config, LoadsAccountsInCurrencyUnits) {
  const AccountConfig c = accounts("ACC1, 2000000, 100000, 50000000\n");
  ASSERT_EQ(c.accounts.size(), 1U);
  EXPECT_EQ(c.accounts.name(0), "ACC1");
  EXPECT_TRUE(c.limits[0].maxOrderNotional == notional(2'000'000));
  EXPECT_EQ(c.limits[0].maxPositionPerSymbol, 100'000);
  EXPECT_TRUE(c.limits[0].maxGrossExposure == notional(50'000'000));
}

TEST(Config, BadLinesAreRefusedWithTheirLineNumber) {
  EXPECT_EQ(errorFrom([] { instruments("# ok\nAAPL, 0.01, 150.00, 500, 10000\n"); }),
            "line 2: expected 6 fields, got 5");
  EXPECT_EQ(errorFrom([] { instruments("AAPL, 0.01, 150.00, 5%, 10000, Y\n"); }), "line 1: bad price band '5%'");
  EXPECT_EQ(errorFrom([] { instruments("AAPL, 0, 150.00, 500, 10000, Y\n"); }), "line 1: bad tick size '0'");
  EXPECT_EQ(errorFrom([] { instruments("AAPL, 0.01, 150.00, 500, 10000, yes\n"); }),
            "line 1: bad tradable flag (Y or N) 'yes'");
  EXPECT_EQ(errorFrom([] { instruments("AAPL, 0.05, 150.01, 500, 10000, Y\n"); }),
            "line 1: bad reference price (not on tick) '150.01'");
  EXPECT_EQ(errorFrom([] { instruments("AAPL,0.01,1,1,1,Y\nAAPL,0.01,1,1,1,Y\n"); }),
            "line 2: bad symbol (listed twice) 'AAPL'");
  EXPECT_EQ(errorFrom([] { accounts("ACC1, 1e6, 100, 100\n"); }), "line 1: bad max order notional '1e6'");
  EXPECT_EQ(errorFrom([] { accounts("ACC1, -5, 100, 100\n"); }), "line 1: bad max order notional '-5'");
}

TEST(Config, ShippedConfigFilesLoad) {
  // The files the gateway runs with must stay valid.
  std::ifstream instrumentsFile(MINI_OEMS_SOURCE_DIR "/config/instruments.csv");
  std::ifstream accountsFile(MINI_OEMS_SOURCE_DIR "/config/accounts.csv");
  ASSERT_TRUE(instrumentsFile && accountsFile);
  EXPECT_GT(loadInstruments(instrumentsFile).symbols.size(), 0U);
  EXPECT_GT(loadAccounts(accountsFile).accounts.size(), 0U);
}

} // namespace
} // namespace oems
