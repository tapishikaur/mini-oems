// mini_oems: the order gateway.
//
//   mini_oems <session config> <instruments.csv> <accounts.csv>
//
// The session config is a QuickFIX settings file. Any "${FIX44_XML}" in it is
// replaced with the path of the FIX 4.4 data dictionary from the pinned QuickFIX
// build. The two CSV files are described in include/oems/config.hpp.

#include "oems/config.hpp"
#include "oems/fix_gateway.hpp"
#include "oems/in_process_venue.hpp"
#include "oems/order_manager.hpp"
#include "oems/risk.hpp"

#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> stopRequested{false};
extern "C" void onSignal(int) { stopRequested = true; }

std::ifstream open(const char* path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error(std::string("can't open ") + path);
  return in;
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: mini_oems <session config> <instruments.csv> <accounts.csv>\n";
    return 2;
  }
  try {
    std::ifstream instrumentsFile = open(argv[2]);
    std::ifstream accountsFile = open(argv[3]);
    const oems::InstrumentConfig instruments = oems::loadInstruments(instrumentsFile);
    const oems::AccountConfig accounts = oems::loadAccounts(accountsFile);

    // Wiring: FIX gateway -> order manager -> risk + venue, and reports back the same way.
    oems::RiskEngine risk(instruments.limits, accounts.limits);
    oems::OrderManager manager(risk);
    oems::InProcessVenue venue(manager);
    oems::FixGateway gateway(manager, instruments.symbols, accounts.accounts);
    manager.connect(venue, gateway);

    std::ifstream settingsFile = open(argv[1]);
    std::stringstream raw;
    raw << settingsFile.rdbuf();
    std::istringstream settingsText(replaceAll(raw.str(), "${FIX44_XML}", MINI_OEMS_FIX44_XML));
    const FIX::SessionSettings settings(settingsText);
    FIX::FileStoreFactory store(settings); // sequence numbers and sent messages, for recovery
    FIX::FileLogFactory log(settings);
    // SocketAcceptor handles every session on one thread, which the order manager requires.
    FIX::SocketAcceptor acceptor(gateway, store, settings, log);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    acceptor.start();
    std::cout << "mini_oems: listening with " << instruments.symbols.size() << " symbols and "
              << accounts.accounts.size() << " accounts. Ctrl+C to stop." << std::endl;
    while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    acceptor.stop();
    std::cout << "mini_oems: stopped.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "mini_oems: " << e.what() << '\n';
    return 1;
  }
}
