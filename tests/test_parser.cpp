#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "itch/parser.hpp"
#include "itch/writer.hpp"

using namespace ts;
using namespace ts::itch;

namespace {

// Remembers the last message of each kind it was given.
struct Recorder : NullHandler {
    SystemEvent system_event;
    StockDirectory directory;
    TradingAction action;
    AddOrder add;
    OrderExecuted executed;
    OrderExecutedWithPrice executed_with_price;
    OrderCancel cancel;
    OrderDelete del;
    OrderReplace replace;
    Trade trade;
    CrossTrade cross;
    std::vector<char> types;

    void on_system_event(const SystemEvent& m) { system_event = m; types.push_back('S'); }
    void on_stock_directory(const StockDirectory& m) { directory = m; types.push_back('R'); }
    void on_trading_action(const TradingAction& m) { action = m; types.push_back('H'); }
    void on_add(const AddOrder& m) { add = m; types.push_back(m.h.type); }
    void on_executed(const OrderExecuted& m) { executed = m; types.push_back('E'); }
    void on_executed_with_price(const OrderExecutedWithPrice& m) { executed_with_price = m; types.push_back('C'); }
    void on_cancel(const OrderCancel& m) { cancel = m; types.push_back('X'); }
    void on_delete(const OrderDelete& m) { del = m; types.push_back('D'); }
    void on_replace(const OrderReplace& m) { replace = m; types.push_back('U'); }
    void on_trade(const Trade& m) { trade = m; types.push_back('P'); }
    void on_cross_trade(const CrossTrade& m) { cross = m; types.push_back('Q'); }
    void on_other(char type, const std::byte*, uint16_t) { types.push_back(type); }
};

ParseResult parse_all(const std::vector<std::byte>& bytes, Recorder& r) {
    return parse(bytes.data(), bytes.size(), r);
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (int v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

constexpr Timestamp kTs = 0x010203040506;  // a 48-bit value with a distinct byte in each place

}  // namespace

TEST(BigEndian, Loads) {
    auto b = bytes_of({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(load_u16(b.data()), 0x0102u);
    EXPECT_EQ(load_u32(b.data()), 0x01020304u);
    EXPECT_EQ(load_u48(b.data()), 0x010203040506u);
    EXPECT_EQ(load_u64(b.data()), 0x0102030405060708u);
}

// An Add Order typed in byte by byte from the spec, so the tests don't only
// check the Writer against itself.
TEST(Parser, AddOrderFromLiteralBytes) {
    auto bytes = bytes_of({
        0x00, 0x24,                                      // length 36
        'A',                                             // type
        0x00, 0x0D,                                      // stock locate 13
        0x00, 0x00,                                      // tracking number
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,              // timestamp
        0x00, 0x00, 0x00, 0x00, 0x00, 0xAB, 0xCD, 0xEF,  // order reference number
        'B',                                             // buy
        0x00, 0x00, 0x00, 0x64,                          // 100 shares
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',          // stock
        0x00, 0x12, 0xD6, 0x44,                          // price 1234500 = $123.4500
    });
    Recorder r;
    auto result = parse_all(bytes, r);
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_EQ(result.messages, 1u);
    EXPECT_EQ(r.add.h.type, 'A');
    EXPECT_EQ(r.add.h.locate, 13);
    EXPECT_EQ(r.add.h.timestamp, kTs);
    EXPECT_EQ(r.add.ref, 0xABCDEFu);
    EXPECT_EQ(r.add.side, Side::Buy);
    EXPECT_EQ(r.add.shares, 100u);
    EXPECT_EQ(r.add.stock.view(), "AAPL");
    EXPECT_EQ(r.add.price, 1234500u);

    Writer w;
    w.add(13, kTs, 0xABCDEF, Side::Buy, 100, 1234500, "AAPL");
    EXPECT_EQ(w.bytes(), bytes);
}

TEST(Parser, SystemEvent) {
    Writer w;
    w.system_event(kTs, 'Q');
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.system_event.h.timestamp, kTs);
    EXPECT_EQ(r.system_event.event_code, 'Q');
}

TEST(Parser, StockDirectory) {
    Writer w;
    w.stock_directory(7, kTs, "MSFT", 100);
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.directory.h.locate, 7);
    EXPECT_EQ(r.directory.stock.view(), "MSFT");
    EXPECT_EQ(r.directory.market_category, 'Q');
    EXPECT_EQ(r.directory.round_lot_size, 100u);
    EXPECT_EQ(r.directory.authenticity, 'P');
}

TEST(Parser, TradingAction) {
    Writer w;
    w.trading_action(9, kTs, "INTC", 'H');
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.action.stock.view(), "INTC");
    EXPECT_EQ(r.action.state, 'H');
}

TEST(Parser, AddOrderWithMpid) {
    Writer w;
    w.add_mpid(3, kTs, 42, Side::Sell, 250, 99, "NITE", "XYZ");
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.add.h.type, 'F');
    EXPECT_EQ(r.add.ref, 42u);
    EXPECT_EQ(r.add.side, Side::Sell);
    EXPECT_EQ(r.add.shares, 250u);
    EXPECT_EQ(r.add.price, 99u);
    EXPECT_EQ(std::string(r.add.mpid.data(), 4), "NITE");
}

TEST(Parser, Executions) {
    Writer w;
    w.executed(1, kTs, 77, 30, 0x1122334455667788);
    w.executed_with_price(1, kTs + 1, 78, 40, 99, 'N', 500100);
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.executed.ref, 77u);
    EXPECT_EQ(r.executed.shares, 30u);
    EXPECT_EQ(r.executed.match, 0x1122334455667788u);
    EXPECT_EQ(r.executed_with_price.ref, 78u);
    EXPECT_EQ(r.executed_with_price.shares, 40u);
    EXPECT_EQ(r.executed_with_price.match, 99u);
    EXPECT_EQ(r.executed_with_price.printable, 'N');
    EXPECT_EQ(r.executed_with_price.price, 500100u);
    EXPECT_EQ(r.executed_with_price.h.timestamp, kTs + 1);
}

TEST(Parser, CancelDeleteReplace) {
    Writer w;
    w.cancel(2, kTs, 5, 60);
    w.del(2, kTs, 6);
    w.replace(2, kTs, 7, 8, 300, 101200);
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.cancel.ref, 5u);
    EXPECT_EQ(r.cancel.shares, 60u);
    EXPECT_EQ(r.del.ref, 6u);
    EXPECT_EQ(r.replace.old_ref, 7u);
    EXPECT_EQ(r.replace.new_ref, 8u);
    EXPECT_EQ(r.replace.shares, 300u);
    EXPECT_EQ(r.replace.price, 101200u);
}

TEST(Parser, Trades) {
    Writer w;
    w.trade(4, kTs, 500, 123400, 31, "QQQ");
    w.cross_trade(4, kTs, 1'000'000'000'000ull, 123500, 32, 'C', "QQQ");
    Recorder r;
    ASSERT_EQ(parse_all(w.bytes(), r).status, Status::Ok);
    EXPECT_EQ(r.trade.shares, 500u);
    EXPECT_EQ(r.trade.price, 123400u);
    EXPECT_EQ(r.trade.match, 31u);
    EXPECT_EQ(r.trade.stock.view(), "QQQ");
    EXPECT_EQ(r.cross.shares, 1'000'000'000'000ull);
    EXPECT_EQ(r.cross.price, 123500u);
    EXPECT_EQ(r.cross.cross_type, 'C');
}

TEST(Parser, SkipsTypesItDoesNotNeedByLength) {
    Writer w;
    w.raw('Y', 1, kTs, std::string(9, 'x'));  // Reg SHO, 20 bytes
    w.raw('z', 1, kTs, std::string(30, 'x')); // a type the spec doesn't define
    w.system_event(kTs, 'C');
    Recorder r;
    auto result = parse_all(w.bytes(), r);
    ASSERT_EQ(result.status, Status::Ok);
    EXPECT_EQ(r.types, (std::vector<char>{'Y', 'z', 'S'}));
}

TEST(Parser, RejectsWrongLengthForKnownType) {
    Writer w;
    w.raw('D', 1, kTs, std::string(9, 'x'));  // a delete is 19 bytes, this is 20
    Recorder r;
    EXPECT_EQ(parse_all(w.bytes(), r).status, Status::BadLength);
    EXPECT_TRUE(r.types.empty());
}

TEST(Parser, RejectsLengthShorterThanHeader) {
    auto bytes = bytes_of({0x00, 0x05, 'S', 0, 0, 0, 0});
    Recorder r;
    EXPECT_EQ(parse_all(bytes, r).status, Status::BadLength);
}

TEST(Parser, RejectsBadSide) {
    Writer w;
    w.add(1, kTs, 1, Side::Buy, 100, 100);
    auto bytes = w.bytes();
    bytes[2 + 19] = static_cast<std::byte>('Z');
    Recorder r;
    EXPECT_EQ(parse_all(bytes, r).status, Status::BadField);
}

// Every prefix of a valid stream: the parser stops with an error inside a
// message and never reads past the end (each prefix is copied into a buffer
// of exactly its size, so AddressSanitizer would catch an over-read).
TEST(Parser, TruncatedInputNeverReadsPastTheEnd) {
    Writer w;
    w.system_event(kTs, 'O');
    w.add(1, kTs, 1, Side::Buy, 100, 100);
    w.executed(1, kTs, 1, 50, 1);
    w.replace(1, kTs, 1, 2, 10, 200);
    w.trade(1, kTs, 5, 100, 2);
    const auto& full = w.bytes();
    const std::vector<size_t> boundaries = {0, 14, 52, 85, 122, 168};
    ASSERT_EQ(full.size(), boundaries.back());

    for (size_t n = 0; n <= full.size(); ++n) {
        auto buf = std::make_unique<std::byte[]>(n == 0 ? 1 : n);
        std::copy(full.begin(), full.begin() + static_cast<long>(n), buf.get());
        Recorder r;
        auto result = parse(buf.get(), n, r);
        bool at_boundary = std::find(boundaries.begin(), boundaries.end(), n) != boundaries.end();
        if (at_boundary) {
            EXPECT_EQ(result.status, Status::Ok) << n;
        } else {
            EXPECT_EQ(result.status, Status::Truncated) << n;
        }
        EXPECT_LE(result.offset, n);
    }
}

TEST(Cursor, ReadsTimestampWithoutDecoding) {
    Writer w;
    w.add(5, kTs, 1, Side::Buy, 100, 100);
    Cursor c(w.bytes().data(), w.bytes().size());
    RawMessage m;
    ASSERT_TRUE(c.next(m));
    EXPECT_EQ(m.type(), 'A');
    EXPECT_EQ(m.locate(), 5);
    EXPECT_EQ(m.timestamp(), kTs);
    EXPECT_FALSE(c.next(m));
    EXPECT_EQ(c.status(), Status::Ok);
}
