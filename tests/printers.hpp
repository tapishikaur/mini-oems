#pragma once
// Readable GoogleTest failure messages for project types. They're in namespace
// oems so GoogleTest finds them, and inline because several test files use them.

#include "oems/order.hpp"
#include "oems/order_book.hpp"
#include "oems/order_manager.hpp"
#include "oems/risk.hpp"

#include <ostream>

namespace oems {

inline void PrintTo(OrdState state, std::ostream* os) { *os << toString(state); }
inline void PrintTo(ApplyResult result, std::ostream* os) { *os << toString(result); }
inline void PrintTo(RiskReject reject, std::ostream* os) { *os << toString(reject); }
inline void PrintTo(ExecType type, std::ostream* os) { *os << static_cast<char>(type); }

inline void PrintTo(const Order& o, std::ostream* os) {
  *os << '{' << toString(o.state()) << " qty=" << o.orderQty() << " cum=" << o.cumQty()
      << " px=" << formatPrice(o.price()) << '}';
}

inline void PrintTo(const Execution& e, std::ostream* os) {
  *os << "{resting=" << e.restingId << " " << e.qty << " @ " << formatPrice(e.price) << '}';
}

} // namespace oems
