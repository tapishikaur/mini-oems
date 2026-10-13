#pragma once
// Drop copy: a copy of every fill, sent to systems other than the one that placed the
// order (risk, back office, a position service). Brokers and exchanges offer the same.
//
// DropCopySink sits between the order manager and the FIX gateway: it passes every report
// on unchanged and also records each fill (ExecType F) in a DropCopyJournal, numbering
// them seq 1, 2, 3, ... DropCopyServer serves the journal over TCP on its own thread, in
// PosTrack's protocol (github.com/tapishikaur/postrack, docs/protocol.md, version 1): a
// client sends Resume{last seq} and gets every fill after it, then new ones as they happen.
// The strategy of each fill is the order's account.

#include "oems/directory.hpp"
#include "oems/order_manager.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oems {

struct DropCopyFill {
  std::uint64_t seq = 0;
  std::uint64_t execId = 0;
  std::int64_t tsNs = 0;
  std::string strategy; // the account
  std::string symbol;
  Side side = Side::Buy;
  Qty qty = 0;
  Price price;
};

// Every fill so far. Appended to by the order manager's thread, read by the server's.
class DropCopyJournal {
public:
  // Appends and returns the fill's seq.
  std::uint64_t append(DropCopyFill fill);
  // Fills with seq > after, at most `max` of them.
  std::vector<DropCopyFill> after(std::uint64_t after, std::size_t max) const;
  std::uint64_t lastSeq() const;

private:
  mutable std::mutex mutex_;
  std::vector<DropCopyFill> fills_; // fills_[i].seq == i + 1
};

class DropCopySink final : public ReportSink {
public:
  DropCopySink(ReportSink& next, DropCopyJournal& journal, const Directory& symbols, const Directory& accounts)
      : next_(next), journal_(journal), symbols_(symbols), accounts_(accounts) {}

  void onExecutionReport(const ExecutionReport& report) override;
  void onCancelReject(const CancelReject& reject) override { next_.onCancelReject(reject); }

private:
  ReportSink& next_;
  DropCopyJournal& journal_;
  const Directory& symbols_;
  const Directory& accounts_;
};

// Serves the journal to any number of clients, each from its own Resume point. Runs its
// own thread; the destructor stops it.
class DropCopyServer {
public:
  DropCopyServer(const DropCopyJournal& journal, std::uint16_t port); // 0 picks a free port
  ~DropCopyServer();
  DropCopyServer(const DropCopyServer&) = delete;
  DropCopyServer& operator=(const DropCopyServer&) = delete;

  std::uint16_t port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// PosTrack frames, for the server and for tests: [u32 length][u8 type][u8 version][fields].
std::string encodeDropCopyFill(const DropCopyFill& fill);
// Decodes one Fill frame body; returns false if it isn't a version-1 Fill.
bool decodeDropCopyFill(const std::string& body, DropCopyFill& out);

} // namespace oems
