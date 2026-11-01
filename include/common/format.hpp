#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ts {

// 1234567 -> "1,234,567"
inline std::string with_commas(uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

inline std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

// 123400 -> "12.3400"
inline std::string dollars(int64_t price) {
    char buf[64];
    const char* sign = price < 0 ? "-" : "";
    uint64_t a = static_cast<uint64_t>(price < 0 ? -price : price);
    std::snprintf(buf, sizeof(buf), "%s%llu.%04llu", sign, static_cast<unsigned long long>(a / 10000),
                  static_cast<unsigned long long>(a % 10000));
    return buf;
}

// nanoseconds since midnight -> "09:30:00.000123456"
inline std::string clock_time(uint64_t ns) {
    char buf[64];
    uint64_t s = ns / 1'000'000'000;
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000));
    return buf;
}

}  // namespace ts
