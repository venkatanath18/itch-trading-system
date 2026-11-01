#pragma once

#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "book/fast_book.hpp"
#include "book/reference_book.hpp"
#include "book/sorted_levels.hpp"
#include "book/tick_levels.hpp"

using FastHashSorted = ts::FastBook<ts::HashIndex, ts::SortedLevels>;
using FastFlatSorted = ts::FastBook<ts::FlatIndex, ts::SortedLevels>;
using FastHashTick = ts::FastBook<ts::HashIndex, ts::TickLevels>;
using FastLocalHashSorted = ts::FastBook<ts::LocalHashIndex, ts::SortedLevels>;
using FastHashTick64 = ts::FastBook<ts::HashIndex, ts::TickLevels64>;

inline ts::BookSizing test_sizing() {
    ts::BookSizing s;
    s.stocks = 16;
    s.max_orders = 1 << 14;
    s.max_levels = 1 << 13;
    s.max_ref = 1 << 20;
    s.side_levels.assign(s.stocks, {256, 256});
    return s;
}

template <class Book>
std::unique_ptr<Book> make_book() {
    if constexpr (std::is_constructible_v<Book, const ts::BookSizing&>) {
        return std::make_unique<Book>(test_sizing());
    } else {
        return std::make_unique<Book>();
    }
}

using BookTypes = ::testing::Types<ts::ReferenceBook, FastHashSorted, FastFlatSorted, FastLocalHashSorted, FastHashTick, FastHashTick64>;
