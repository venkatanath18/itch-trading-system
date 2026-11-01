#pragma once

#include "itch/messages.hpp"

namespace ts::itch {

// The stages of the day, from System Event messages:
//   O start of messages, S start of system hours, Q start of market hours,
//   M end of market hours, E end of system hours, C end of messages.
class Session {
public:
    void on_system_event(const SystemEvent& e) {
        last_ = e.event_code;
        if (e.event_code == 'Q') regular_ = true;
        if (e.event_code == 'M') regular_ = false;
    }

    // Between the Q and M events (9:30 to 16:00).
    bool regular_hours() const { return regular_; }
    char last_event() const { return last_; }

private:
    char last_ = 0;
    bool regular_ = false;
};

}  // namespace ts::itch
