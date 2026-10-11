# MiniOEMS

A FIX 4.4 order gateway in C++20 with pre-trade risk checks, an explicit order-lifecycle state machine, and an in-process limit order book.

> **Status: work in progress.** This README describes the design. Anything marked *planned* is not implemented yet, and the results table shows only numbers that have actually been measured.

## What it does

MiniOEMS is the core of an order and execution management system (OEMS). Clients connect over FIX 4.4 and send orders. Each order is:

1. **Received and validated.** QuickFIX handles the FIX session layer: logon, heartbeats, sequence numbers and resends.
2. **Risk-checked** before it goes anywhere.
3. **Tracked** by a state machine that refuses illegal transitions.
4. **Matched** in a price-time-priority limit order book.
5. **Reported back** with an ExecutionReport for every change.

## Architecture

```mermaid
flowchart LR
    C["Client<br/>(FIX initiator)"] -- "35=D / F / G" --> Q["QuickFIX acceptor<br/>(session layer)"]
    Q --> G["FIX gateway<br/>FIX ⇄ internal types"]
    G --> R["Pre-trade risk"]
    R -- "rejected" --> G
    R -- "accepted" --> M["Order manager<br/>state machine + positions"]
    M <--> B["Limit order book<br/>price-time priority"]
    M -- "acks, fills" --> G
    G -- "35=8 / 35=9" --> Q
    Q --> C
```

The gateway, risk engine, order manager and order book run on a single thread (QuickFIX's `SocketAcceptor`), so the core needs no locks and is deterministic: the same input always produces the same output.

## Order lifecycle

```mermaid
stateDiagram-v2
    [*] --> PendingNew: NewOrderSingle (35=D)
    PendingNew --> Rejected: fails a risk check
    PendingNew --> New: accepted
    New --> PartiallyFilled: partial fill
    New --> Filled: full fill
    PartiallyFilled --> Filled: remaining quantity filled
    New --> PendingCancel: cancel (35=F)
    PartiallyFilled --> PendingCancel: cancel (35=F)
    PendingCancel --> Canceled: cancel done
    PendingCancel --> Filled: filled first (cancel rejected, 35=9)
    New --> PendingReplace: cancel/replace (35=G)
    PartiallyFilled --> PendingReplace: cancel/replace (35=G)
    PendingReplace --> New: replace done
    PendingReplace --> PartiallyFilled: replace done
    PendingReplace --> Filled: filled first (replace rejected, 35=9)
    Rejected --> [*]
    Canceled --> [*]
    Filled --> [*]
```

Any transition not in this table is rejected and leaves the order unchanged. For example, a fill on a canceled order is refused. In FIX 4.4 a successful replace is reported with `ExecType=5` (Replaced), while `OrdStatus` shows the order's current state.

## Pre-trade risk checks

These checks run in order, cheapest first, and stop at the first failure:

1. The account and symbol are known, and the symbol is open for trading.
2. Quantity is above zero and within the per-symbol maximum.
3. Price is positive and on the tick grid.
4. Price is within a band around the symbol's reference price, to catch "fat finger" errors.
5. Notional (price × quantity) is within the per-account maximum. It's computed in 128 bits, so a huge order can't overflow and slip under the limit.
6. The worst-case position in the symbol is within the account's limit. Worst case means current position, plus every open order on the same side, plus this order. Otherwise ten orders, each under the limit, could breach it together.
7. Gross exposure across all symbols is within the account's limit. This is worst-case positions valued at reference prices.

An order that doesn't increase the worst case is always allowed, even if the account is already over a limit (for example, after the limit was lowered during the day). That covers a sell while long, or a replace that shrinks an order. Cancels are never checked, because reducing risk must always be possible. A cancel/replace is re-checked against its new price and quantity.

Limits and exposures sit in flat arrays indexed by small integer IDs, so a check does no hashing, no memory allocation and no locking.

## FIX messages

| Message | `35=` | Direction |
|---|---|---|
| NewOrderSingle | `D` | in |
| OrderCancelRequest | `F` | in |
| OrderCancelReplaceRequest | `G` | in |
| ExecutionReport (new, partial fill, fill, canceled, replaced, rejected) | `8` | out |
| OrderCancelReject | `9` | out |

Any other message type gets a BusinessMessageReject (`35=j`). Prices and quantities are read from the raw FIX text and parsed exactly, so no value ever passes through a `double`. A price with more than 4 decimal places is rejected, not rounded.

### Duplicates, resends and gaps

| Situation | What happens |
|---|---|
| A `ClOrdID` is reused | Rejected: `OrdRejReason=6` for a new order, `CxlRejReason=6` for a cancel/replace. No second order is created. ClOrdIDs are unique per FIX session. |
| A `NewOrderSingle` arrives again with `PossResend=Y` or `PossDupFlag=Y`, and we already have it | A status report (`ExecType=I`) with the order's current state. Nothing changes. |
| The same flag, but we've never seen that `ClOrdID` | Processed as a new order, because the original was lost. |
| A copy with `PossDupFlag=Y` and a sequence number we've already processed | Dropped by QuickFIX's session layer; the application never sees it. |
| A message arrives out of sequence (a gap) | QuickFIX holds it, asks the client to resend the missing range (`35=2`), processes the resent messages in order, then the held one, exactly once. |
| A cancel or replace arrives before the client saw the order's acknowledgement | Requests are handled in arrival order, so the order is already accepted and the cancel finds it. |
| A cancel names an unknown order, or one that's already done | `OrderCancelReject` with `CxlRejReason=1` (unknown) or `0` (too late). |

All of these are tested over a real localhost FIX connection, with QuickFIX on both ends (`tests/fix_gateway_test.cpp`).

## Results

| Metric | Design target | Measured | How measured |
|---|---|---|---|
| Risk-check latency | ~1 µs per order | **7.4 ns** median (passes every check); 1.5 ns when rejected by the first check; 14.1 ns including exposure updates for an accepted, filled order | `bench/risk_benchmark` (Google Benchmark, 10 repetitions, median), over 4,096 varied orders across 500 symbols and 50 accounts. Intel Core Ultra 7 165U laptop, Windows 11, GCC 16.1, Release build. |
| End-to-end throughput (local FIX sessions) | 20,000 orders/s | not yet measured | |
| Test cases | 120 | not yet final (227 so far) | |

All numbers come with hardware, OS and method. A missed target stays in the table with an explanation.

The risk number is the check measured on its own, in a tight loop with warm caches. Inside a real order flow it competes with FIX parsing and the network for cache space, and the end-to-end breakdown will show its real share. The laptop numbers will be re-measured on a 2-core Linux VM.

## Tech stack

| Component | Choice |
|---|---|
| Language | C++20 |
| FIX engine | [QuickFIX](https://github.com/quickfix/quickfix), built from source at a pinned commit |
| Order book | Written from scratch for this project |
| Build | CMake |
| Tests | GoogleTest |
| Microbenchmarks | Google Benchmark |
| CI | GitHub Actions on Ubuntu, Release and Debug builds (configured; runs once the repository is on GitHub) |

## Why these choices

- **QuickFIX for the session layer.** Sequence numbers, gap detection, resends and heartbeats are easy to get subtly wrong. QuickFIX is a mature open-source engine, which leaves this project free to focus on order handling and risk.
- **Fixed-point prices.** Prices are stored as integers (for example, 10.25 becomes 102500). Floating-point rounding has no place in money calculations.
- **Explicit transition table.** Illegal order states can't occur by accident, and every transition is testable.
- **Single-threaded core.** No locks, deterministic behaviour, and easy replay of any scenario.
- **FIX only at the edge.** The order manager, risk engine, order book and state machine don't know FIX exists. `FixGateway` is the only code that touches QuickFIX, so the core is tested without sockets, and swapping the protocol wouldn't touch it.
- **Every new ClOrdID gets an order, even one about to be rejected.** The client always gets an OrderID back, and a rejected ClOrdID can't be quietly reused.
- **Testing against simple reference models.** The order book is checked against a flat-list book that's slow but obviously correct, over 200,000 random operations. The risk engine's incrementally updated exposure is checked against a full recount, over 100,000 random operations. The state machine gets 250,000 random events. Seeds are fixed, so every run is identical. To prove the tests can fail, deliberate bugs were planted in each component, and every one was caught by at least two tests.

## Building

You need Git, bash, CMake 3.25 or newer, Ninja, and a C++20 compiler.

- **Linux:** `sudo apt install build-essential cmake ninja-build git`
- **Windows:** MSYS2 with the MinGW-w64 toolchain (`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-ninja`). Run the script from Git Bash or an MSYS2 shell.

```bash
bash scripts/build.sh            # Release build; Debug: bash scripts/build.sh Debug
```

This single command does three things:

1. Builds QuickFIX from its pinned commit, the first time only (a few minutes).
2. Builds MiniOEMS.
3. Runs the tests.

Build output goes to `build/`. On Windows, if the repository sits inside a OneDrive folder, output goes to `%LOCALAPPDATA%\mini-oems\build` instead, because OneDrive locks the build folders it syncs. Set `MINI_OEMS_BUILD_ROOT` to choose another location.

QuickFIX doesn't officially support MinGW. `scripts/build_quickfix.sh` applies [`third_party/quickfix/mingw.patch`](third_party/quickfix/), which makes MinGW use QuickFIX's existing Windows (Winsock) code paths. The patch has no effect on Linux. On Windows, QuickFIX's own acceptance test suite passes in full with the patch applied.

## Running the gateway

```bash
mini_oems config/acceptor.cfg config/instruments.csv config/accounts.csv
```

The executable is in the build output folder.
- **`config/acceptor.cfg`** is a QuickFIX settings file. It listens on port 9878 for FIX 4.4 sessions `CLIENT1` and `CLIENT2`.
- **`config/instruments.csv`** sets each symbol's tick size, reference price, price band, maximum order size and whether it's tradable.
- **`config/accounts.csv`** sets each account's maximum order notional, maximum position per symbol and maximum gross exposure.

A malformed line in either CSV stops startup with the line number. A typo is never silently read as zero.

## Limitations

- Limit orders only (Day and IOC time-in-force).
- Reference prices come from a config file, not a live market-data feed.
- Order state is held in memory. QuickFIX persists session messages, but order state isn't recovered after a restart.
- A single venue: an in-process order book with no smart order routing.
- No self-trade prevention: two orders from the same account can trade with each other.
- No pending-state reports. Cancels and replaces complete within the request, so clients get the final Canceled/Replaced report, never PendingCancel or PendingReplace.
- The sample session config resets sequence numbers on every logon (`ResetOnLogon=Y`), which keeps demos simple. A production setup keeps them for the whole trading day.

## License

This project's code is released under the [MIT License](LICENSE).

QuickFIX is distributed under the QuickFIX Software License, Version 1.0, a BSD-style licence. See the QuickFIX repository for its terms.
