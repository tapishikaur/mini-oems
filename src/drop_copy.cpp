#include "oems/drop_copy.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <thread>

namespace oems {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

constexpr std::uint8_t kFill = 2;
constexpr std::uint8_t kResume = 1;
constexpr std::uint8_t kVersion = 1;

void putU8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
void putU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putU64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putStr(std::string& out, const std::string& s) {
  putU8(out, static_cast<std::uint8_t>(s.size()));
  out += s;
}
std::uint64_t getU64(const std::string& s, std::size_t at) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
  return v;
}

} // namespace

std::uint64_t DropCopyJournal::append(DropCopyFill fill) {
  std::lock_guard lock(mutex_);
  fill.seq = fills_.size() + 1;
  fills_.push_back(std::move(fill));
  return fills_.back().seq;
}

std::vector<DropCopyFill> DropCopyJournal::after(std::uint64_t after, std::size_t max) const {
  std::lock_guard lock(mutex_);
  std::vector<DropCopyFill> out;
  for (std::uint64_t i = after; i < fills_.size() && out.size() < max; ++i) out.push_back(fills_[i]);
  return out;
}

std::uint64_t DropCopyJournal::lastSeq() const {
  std::lock_guard lock(mutex_);
  return fills_.size();
}

void DropCopySink::onExecutionReport(const ExecutionReport& report) {
  if (report.execType == ExecType::Trade && accounts_.contains(report.account) && symbols_.contains(report.symbol)) {
    DropCopyFill f;
    f.execId = report.execId;
    f.tsNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                 .count();
    f.strategy = accounts_.name(report.account);
    f.symbol = symbols_.name(report.symbol);
    f.side = report.side;
    f.qty = report.lastQty;
    f.price = report.lastPx;
    journal_.append(std::move(f));
  }
  next_.onExecutionReport(report);
}

std::string encodeDropCopyFill(const DropCopyFill& fill) {
  std::string body;
  putU8(body, kFill);
  putU8(body, kVersion);
  putU64(body, fill.seq);
  putU64(body, fill.execId);
  putU64(body, static_cast<std::uint64_t>(fill.tsNs));
  putStr(body, fill.strategy);
  putStr(body, fill.symbol);
  putU8(body, fill.side == Side::Buy ? 1 : 2);
  putU64(body, static_cast<std::uint64_t>(fill.qty));
  putU64(body, static_cast<std::uint64_t>(fill.price.raw()));
  std::string frame;
  putU32(frame, static_cast<std::uint32_t>(body.size()));
  return frame + body;
}

bool decodeDropCopyFill(const std::string& body, DropCopyFill& out) {
  std::size_t at = 0;
  const auto need = [&](std::size_t n) { return body.size() - at >= n; };
  if (!need(2) || static_cast<std::uint8_t>(body[0]) != kFill || static_cast<std::uint8_t>(body[1]) != kVersion) {
    return false;
  }
  at = 2;
  if (!need(24)) return false;
  out.seq = getU64(body, at);
  out.execId = getU64(body, at + 8);
  out.tsNs = static_cast<std::int64_t>(getU64(body, at + 16));
  at += 24;
  for (std::string* s : {&out.strategy, &out.symbol}) {
    if (!need(1)) return false;
    const std::size_t n = static_cast<unsigned char>(body[at++]);
    if (!need(n)) return false;
    *s = body.substr(at, n);
    at += n;
  }
  if (!need(17)) return false;
  out.side = body[at] == 1 ? Side::Buy : Side::Sell;
  out.qty = static_cast<Qty>(getU64(body, at + 1));
  out.price = Price::fromRaw(static_cast<std::int64_t>(getU64(body, at + 9)));
  return at + 17 == body.size();
}

// ── Server ───────────────────────────────────────────────────────────────────

struct DropCopyServer::Impl {
  struct Session : std::enable_shared_from_this<Session> {
    Session(const DropCopyJournal& j, tcp::socket s) : journal(j), socket(std::move(s)), idle(socket.get_executor()) {}

    const DropCopyJournal& journal;
    tcp::socket socket;
    asio::steady_timer idle;
    std::array<char, 14> resume{}; // [u32 10][u8 1][u8 1][u64 last seq]
    std::string out;
    std::uint64_t sent = 0;

    void start() {
      auto self = shared_from_this();
      asio::async_read(socket, asio::buffer(resume), [self](boost::system::error_code ec, std::size_t) {
        if (ec) return;
        const std::string r(self->resume.data(), self->resume.size());
        if (static_cast<unsigned char>(r[0]) != 10 || static_cast<std::uint8_t>(r[4]) != kResume ||
            static_cast<std::uint8_t>(r[5]) != kVersion) {
          return; // not a Resume: drop the connection
        }
        self->sent = getU64(r, 6);
        self->sendMore();
      });
    }

    void sendMore() {
      out.clear();
      for (const DropCopyFill& f : journal.after(sent, 512)) {
        out += encodeDropCopyFill(f);
        sent = f.seq;
      }
      auto self = shared_from_this();
      if (out.empty()) { // nothing new yet: look again shortly
        idle.expires_after(std::chrono::milliseconds(1));
        idle.async_wait([self](boost::system::error_code ec) {
          if (!ec) self->sendMore();
        });
        return;
      }
      asio::async_write(socket, asio::buffer(out), [self](boost::system::error_code ec, std::size_t) {
        if (!ec) self->sendMore();
      });
    }
  };

  Impl(const DropCopyJournal& j, std::uint16_t port)
      : journal(j), acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)) {
    accept();
    thread = std::thread([this] { io.run(); });
  }
  ~Impl() {
    io.stop();
    if (thread.joinable()) thread.join();
  }

  void accept() {
    acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
      if (ec) return;
      std::make_shared<Session>(journal, std::move(socket))->start();
      accept();
    });
  }

  const DropCopyJournal& journal;
  asio::io_context io;
  tcp::acceptor acceptor;
  std::thread thread;
};

DropCopyServer::DropCopyServer(const DropCopyJournal& journal, std::uint16_t port)
    : impl_(std::make_unique<Impl>(journal, port)) {}
DropCopyServer::~DropCopyServer() = default;
std::uint16_t DropCopyServer::port() const { return impl_->acceptor.local_endpoint().port(); }

} // namespace oems
