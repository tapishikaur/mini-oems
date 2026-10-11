// mini_oems: the order gateway.
//
//   mini_oems <session config> <instruments.csv> <accounts.csv>
//
// The session config is a QuickFIX settings file (see oems/fix_settings.hpp for
// ${FIX44_XML}). With FileStorePath set, sequence numbers and sent messages are
// kept on disk for recovery; without it, in memory. With FileLogPath set, every
// message is also logged to disk. The two CSV files are described in
// include/oems/config.hpp.

#include "oems/config.hpp"
#include "oems/fix_gateway.hpp"
#include "oems/fix_settings.hpp"
#include "oems/in_process_venue.hpp"
#include "oems/order_manager.hpp"
#include "oems/risk.hpp"

#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/MessageStore.h>
#include <quickfix/SocketAcceptor.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> stopRequested{false};
extern "C" void onSignal(int) { stopRequested = true; }

std::ifstream open(const char* path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error(std::string("can't open ") + path);
  return in;
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

    const FIX::SessionSettings settings = oems::loadSessionSettings(argv[1]);
    const FIX::Dictionary& defaults = settings.get();
    std::unique_ptr<FIX::MessageStoreFactory> store;
    if (defaults.has("FileStorePath")) {
      store = std::make_unique<FIX::FileStoreFactory>(settings);
    } else {
      store = std::make_unique<FIX::MemoryStoreFactory>();
    }
    std::unique_ptr<FIX::LogFactory> log;
    if (defaults.has("FileLogPath")) log = std::make_unique<FIX::FileLogFactory>(settings);

    // SocketAcceptor handles every session on one thread, which the order manager requires.
    std::unique_ptr<FIX::SocketAcceptor> acceptor =
        log ? std::make_unique<FIX::SocketAcceptor>(gateway, *store, settings, *log)
            : std::make_unique<FIX::SocketAcceptor>(gateway, *store, settings);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    acceptor->start();
    std::cout << "mini_oems: listening with " << instruments.symbols.size() << " symbols and "
              << accounts.accounts.size() << " accounts" << (log ? ", logging messages" : "")
              << ". Ctrl+C to stop." << std::endl;
    while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    acceptor->stop();
    std::cout << "mini_oems: stopped." << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "mini_oems: " << e.what() << '\n';
    return 1;
  }
}
