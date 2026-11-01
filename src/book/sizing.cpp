#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "book/fast_book.hpp"

namespace ts {

BookSizing BookSizing::from_profile(const std::string& sizing_csv, const std::string& global_txt,
                                    double headroom) {
    std::ifstream global(global_txt);
    if (!global) throw std::runtime_error("cannot read " + global_txt + " (run day_summary --global first)");
    uint64_t peak_orders = 0, max_ref = 0, max_locate = 0;
    std::string key;
    uint64_t value;
    while (global >> key >> value) {
        if (key == "peak_live_orders") peak_orders = value;
        if (key == "max_ref") max_ref = value;
        if (key == "max_locate") max_locate = value;
    }

    std::ifstream csv(sizing_csv);
    if (!csv) throw std::runtime_error("cannot read " + sizing_csv);
    BookSizing s;
    s.stocks = max_locate + 1;
    s.side_levels.assign(s.stocks, {0, 0});
    std::string line;
    std::getline(csv, line);  // header
    uint64_t total_levels = 0;
    while (std::getline(csv, line)) {
        // locate,symbol,adds,min_price,max_price,peak_orders,peak_bid_levels,peak_ask_levels,...
        std::stringstream row(line);
        std::string field[8];
        for (auto& f : field) std::getline(row, f, ',');
        const size_t locate = std::stoul(field[0]);
        if (locate >= s.stocks) continue;
        for (int side = 0; side < 2; ++side) {
            const auto peak = std::stoul(field[6 + side]);
            const auto reserve = static_cast<uint32_t>(std::ceil(static_cast<double>(peak) * headroom)) + 8;
            s.side_levels[locate][static_cast<size_t>(side)] = reserve;
            total_levels += peak;
        }
    }
    s.max_orders = static_cast<size_t>(static_cast<double>(peak_orders) * headroom);
    s.max_levels = static_cast<size_t>(static_cast<double>(total_levels) * headroom);
    s.max_ref = max_ref + max_ref / 4;
    return s;
}

}  // namespace ts
