// libFuzzer entry point for the ITCH parser: any input must give a status,
// never a crash or a read past the end (built with ASan + UBSan).
#include <cstddef>
#include <cstdint>

#include "itch/parser.hpp"

namespace {

struct Sink : ts::itch::NullHandler {
    uint64_t sum = 0;
    void on_add(const ts::itch::AddOrder& m) { sum += m.ref + m.price + m.shares + m.h.timestamp; }
    void on_executed(const ts::itch::OrderExecuted& m) { sum += m.ref + m.shares; }
    void on_executed_with_price(const ts::itch::OrderExecutedWithPrice& m) { sum += m.price; }
    void on_cancel(const ts::itch::OrderCancel& m) { sum += m.shares; }
    void on_delete(const ts::itch::OrderDelete& m) { sum += m.ref; }
    void on_replace(const ts::itch::OrderReplace& m) { sum += m.new_ref + m.price; }
    void on_trade(const ts::itch::Trade& m) { sum += m.price; }
    void on_cross_trade(const ts::itch::CrossTrade& m) { sum += m.shares; }
    void on_stock_directory(const ts::itch::StockDirectory& m) { sum += m.round_lot_size; }
    void on_other(char, const std::byte* m, uint16_t len) { sum += static_cast<uint64_t>(m[len - 1]); }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Sink sink;
    auto bytes = reinterpret_cast<const std::byte*>(data);
    auto result = ts::itch::parse(bytes, size, sink);
    if (result.offset > size) __builtin_trap();

    ts::itch::Cursor cursor(bytes, size);
    ts::itch::RawMessage m;
    while (cursor.next(m)) sink.sum += m.timestamp() + m.locate();
    if (cursor.offset() > size) __builtin_trap();
    return 0;
}
