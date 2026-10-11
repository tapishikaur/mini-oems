// Smoke tests: prove the pinned, patched QuickFIX build links and works on this platform.
#include <gtest/gtest.h>

#include <quickfix/Message.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <string>

namespace {

// Price is set from text, not from a double, so the value on the wire is exact.
FIX44::NewOrderSingle makeOrder() {
  FIX44::NewOrderSingle order(FIX::ClOrdID("A1"), FIX::Side(FIX::Side_BUY), FIX::TransactTime(),
                              FIX::OrdType(FIX::OrdType_LIMIT));
  order.set(FIX::Symbol("AAPL"));
  order.set(FIX::OrderQty(100));
  order.setField(FIX::FIELD::Price, "150.25");
  return order;
}

} // namespace

TEST(QuickFixSmoke, SerialisesFix44NewOrderSingle) {
  const std::string wire = makeOrder().toString();

  EXPECT_NE(wire.find("8=FIX.4.4\x01"), std::string::npos);
  EXPECT_NE(wire.find("\x01" "35=D\x01"), std::string::npos);
  EXPECT_NE(wire.find("\x01" "11=A1\x01"), std::string::npos);
  EXPECT_NE(wire.find("\x01" "44=150.25\x01"), std::string::npos);
}

TEST(QuickFixSmoke, ParsesItsOwnOutput) {
  const FIX::Message parsed(makeOrder().toString(), /*validate=*/false);

  EXPECT_EQ(parsed.getHeader().getField(FIX::FIELD::MsgType), "D");
  EXPECT_EQ(parsed.getField(FIX::FIELD::ClOrdID), "A1");
  EXPECT_EQ(parsed.getField(FIX::FIELD::Symbol), "AAPL");
  EXPECT_EQ(parsed.getField(FIX::FIELD::Price), "150.25");
}
