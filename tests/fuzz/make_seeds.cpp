// Writes a few valid ITCH streams as fuzz seeds: make_seeds <dir>
#include <fstream>
#include <string>

#include "itch/writer.hpp"

using namespace ts;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    auto save = [&](const std::string& name, const itch::Writer& w) {
        std::ofstream out(dir + "/" + name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(w.bytes().data()), static_cast<long>(w.bytes().size()));
    };

    itch::Writer day;
    day.system_event(hms(3, 0, 0), 'O');
    day.stock_directory(1, hms(3, 0, 1), "AAPL");
    day.trading_action(1, hms(3, 0, 2), "AAPL", 'T');
    day.system_event(hms(9, 30, 0), 'Q');
    day.add(1, hms(9, 30, 1), 1, Side::Buy, 100, 2900000, "AAPL");
    day.add_mpid(1, hms(9, 30, 2), 2, Side::Sell, 200, 2900100, "NITE", "AAPL");
    day.executed(1, hms(9, 30, 3), 1, 50, 1);
    day.executed_with_price(1, hms(9, 30, 4), 1, 50, 2, 'Y', 2900000);
    day.cancel(1, hms(9, 30, 5), 2, 100);
    day.replace(1, hms(9, 30, 6), 2, 3, 100, 2900200);
    day.del(1, hms(9, 30, 7), 3);
    day.trade(1, hms(9, 30, 8), 100, 2900050, 3, "AAPL");
    day.cross_trade(1, hms(16, 0, 0), 5000, 2900100, 4, 'C', "AAPL");
    day.system_event(hms(16, 0, 0), 'M');
    day.raw('Y', 1, hms(16, 0, 1), std::string(9, 'N'));
    day.system_event(hms(20, 0, 0), 'C');
    save("day.bin", day);

    itch::Writer adds;
    for (uint64_t i = 1; i <= 20; ++i) {
        adds.add(static_cast<Locate>(i % 3), hms(10, 0, 0) + i, i, i % 2 ? Side::Buy : Side::Sell,
                 static_cast<Qty>(100 * i), static_cast<Price>(1000000 + 100 * i));
    }
    save("adds.bin", adds);
    return 0;
}
