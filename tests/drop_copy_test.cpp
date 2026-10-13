#include "oems/drop_copy.hpp"

#include "printers.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>

#include <vector>

namespace oems {
namespace {

class RecordingSink : public ReportSink {
public:
  void onExecutionReport(const ExecutionReport& r) override { reports.push_back(r); }
  void onCancelReject(const CancelReject&) override { ++rejects; }
  std::vector<ExecutionReport> reports;
  int rejects = 0;
};

struct Fixture {
  Fixture() {
    symbols.add("AAPL");
    accounts.add("ACC1");
  }
  Directory symbols, accounts;
  RecordingSink next;
  DropCopyJournal journal;
};

ExecutionReport trade(Qty qty, std::int64_t px, Side side = Side::Buy) {
  ExecutionReport r;
  r.execType = ExecType::Trade;
  r.execId = 77;
  r.side = side;
  r.lastQty = qty;
  r.lastPx = Price::fromRaw(px);
  return r;
}

TEST(DropCopySink, PassesEveryReportOnAndJournalsOnlyFills) {
  Fixture f;
  DropCopySink sink(f.next, f.journal, f.symbols, f.accounts);
  ExecutionReport ack;
  ack.execType = ExecType::New;
  sink.onExecutionReport(ack);
  sink.onExecutionReport(trade(60, 1'502'000, Side::Sell));
  sink.onCancelReject(CancelReject{});

  EXPECT_EQ(f.next.reports.size(), 2u); // both reports reached the gateway, unchanged
  EXPECT_EQ(f.next.rejects, 1);
  ASSERT_EQ(f.journal.lastSeq(), 1u); // only the fill was journaled
  const DropCopyFill j = f.journal.after(0, 10).at(0);
  EXPECT_EQ(j.seq, 1u);
  EXPECT_EQ(j.execId, 77u);
  EXPECT_EQ(j.strategy, "ACC1"); // the account is the strategy
  EXPECT_EQ(j.symbol, "AAPL");
  EXPECT_EQ(j.side, Side::Sell);
  EXPECT_EQ(j.qty, 60);
  EXPECT_EQ(j.price, Price::fromRaw(1'502'000));
}

TEST(DropCopyJournal, NumbersFillsAndReadsFromAnyPoint) {
  DropCopyJournal journal;
  for (int i = 0; i < 10; ++i) journal.append(DropCopyFill{});
  EXPECT_EQ(journal.lastSeq(), 10u);
  const auto tail = journal.after(7, 100);
  ASSERT_EQ(tail.size(), 3u);
  EXPECT_EQ(tail[0].seq, 8u);
  EXPECT_EQ(journal.after(2, 4).size(), 4u);
  EXPECT_TRUE(journal.after(10, 5).empty());
}

TEST(DropCopyWire, FillFramesRoundTrip) {
  DropCopyFill f{5, 99, 123456789, "ACC2", "MSFT", Side::Sell, 250, Price::fromRaw(4'001'500)};
  const std::string frame = encodeDropCopyFill(f);
  ASSERT_GE(frame.size(), 4u);
  EXPECT_EQ(static_cast<std::size_t>(static_cast<unsigned char>(frame[0])), frame.size() - 4);
  DropCopyFill back;
  ASSERT_TRUE(decodeDropCopyFill(frame.substr(4), back));
  EXPECT_EQ(back.seq, f.seq);
  EXPECT_EQ(back.strategy, f.strategy);
  EXPECT_EQ(back.symbol, f.symbol);
  EXPECT_EQ(back.side, f.side);
  EXPECT_EQ(back.qty, f.qty);
  EXPECT_EQ(back.price, f.price);
  EXPECT_FALSE(decodeDropCopyFill(frame.substr(4, 10), back)); // cut short
}

// Reads `count` Fill frames from a socket.
std::vector<DropCopyFill> readFills(boost::asio::ip::tcp::socket& socket, std::size_t count) {
  std::vector<DropCopyFill> out;
  while (out.size() < count) {
    unsigned char len[4];
    boost::asio::read(socket, boost::asio::buffer(len));
    std::string body(static_cast<std::size_t>(len[0] | len[1] << 8 | len[2] << 16 | len[3] << 24), '\0');
    boost::asio::read(socket, boost::asio::buffer(body));
    DropCopyFill f;
    EXPECT_TRUE(decodeDropCopyFill(body, f));
    out.push_back(f);
  }
  return out;
}

std::string resume(std::uint64_t last) {
  std::string frame = {10, 0, 0, 0, 1, 1};
  for (int i = 0; i < 8; ++i) frame.push_back(static_cast<char>((last >> (8 * i)) & 0xFF));
  return frame;
}

TEST(DropCopyServer, ServesFromTheResumePointThenNewFills) {
  DropCopyJournal journal;
  for (int i = 0; i < 5; ++i) journal.append(DropCopyFill{0, 0, 0, "ACC1", "AAPL", Side::Buy, 100 + i, Price::fromUnits(1)});
  DropCopyServer server(journal, 0);

  boost::asio::io_context io;
  boost::asio::ip::tcp::socket socket(io);
  socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
  boost::asio::write(socket, boost::asio::buffer(resume(3))); // "I have 1..3"

  std::vector<DropCopyFill> got = readFills(socket, 2);
  EXPECT_EQ(got[0].seq, 4u);
  EXPECT_EQ(got[1].seq, 5u);

  journal.append(DropCopyFill{0, 0, 0, "ACC1", "AAPL", Side::Sell, 7, Price::fromUnits(2)}); // a new fill
  got = readFills(socket, 1);
  EXPECT_EQ(got[0].seq, 6u);
  EXPECT_EQ(got[0].qty, 7);
}

} // namespace
} // namespace oems
