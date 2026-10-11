#pragma once
// Reads instrument and account limits from small CSV files. Lines that are
// blank or start with '#' are skipped; fields are separated by commas.
//
// instruments: symbol, tick size, reference price, price band (bps), max order qty, tradable (Y/N)
//     AAPL, 0.01, 150.00, 500, 10000, Y
// accounts: account, max order notional, max position per symbol, max gross exposure
//     ACC1, 1000000, 50000, 20000000          (notionals in whole currency units)
//
// Any malformed line throws std::runtime_error naming the line, so a typo in a
// limit can never be silently read as zero.

#include "oems/directory.hpp"
#include "oems/risk.hpp"

#include <istream>
#include <vector>

namespace oems {

struct InstrumentConfig {
  Directory symbols;               // name -> SymbolId
  std::vector<SymbolLimits> limits; // indexed by SymbolId
};

struct AccountConfig {
  Directory accounts;               // name -> AccountId
  std::vector<AccountLimits> limits; // indexed by AccountId
};

InstrumentConfig loadInstruments(std::istream& in);
AccountConfig loadAccounts(std::istream& in);

} // namespace oems
