#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/types.hpp"

namespace ts {

// What the strategy wants to do with one order.
enum class IntentType : uint8_t { New, Cancel, Replace };

struct OrderIntent {
    IntentType type = IntentType::New;
    Locate locate = 0;
    Side side = Side::Buy;
    uint64_t id = 0;      // the new order (New, Replace) or the order to cancel (Cancel)
    uint64_t old_id = 0;  // Replace: the order it replaces
    Price price = 0;
    Qty qty = 0;
};

// What the exchange says back.
enum class ResponseType : uint8_t { Ack, Reject, ReplaceAck, ReplaceReject, CancelAck, CancelReject, Fill };

enum class RejectReason : uint8_t { None, WouldCross, NotLive, Unknown };

struct Response {
    ResponseType type = ResponseType::Ack;
    Locate locate = 0;
    Side side = Side::Buy;
    RejectReason reason = RejectReason::None;
    uint64_t id = 0;      // the order (ReplaceAck: the new one)
    uint64_t old_id = 0;  // ReplaceAck, ReplaceReject: the replaced one
    Price price = 0;      // Fill: the fill price; Ack/ReplaceAck: the order price
    Qty qty = 0;          // Fill: shares filled
    Qty remaining = 0;    // Fill: shares still open on the order
    Timestamp exchange_time = 0;
};

// A small fixed-capacity list, so building a batch of intents never allocates.
template <class T, size_t N>
class FixedList {
public:
    bool push(const T& v) {
        if (size_ == N) return false;
        items_[size_++] = v;
        return true;
    }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const T& operator[](size_t i) const { return items_[i]; }
    void clear() { size_ = 0; }
    const T* begin() const { return items_.data(); }
    const T* end() const { return items_.data() + size_; }

private:
    std::array<T, N> items_{};
    size_t size_ = 0;
};

using IntentList = FixedList<OrderIntent, 8>;

}  // namespace ts
