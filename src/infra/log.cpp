#include "infra/log.hpp"

#include "common/machine.hpp"

namespace ts {

void ThreadedLog::drain(int core) {
    if (core >= 0) pin_this_thread(core);
    LogRecord r;
    for (;;) {
        if (queue_->try_pop(r)) {
            writer_.write(r);
            continue;
        }
        if (done_.load(std::memory_order_acquire)) {
            // the producer set done_ after its last push, so everything is visible now
            while (queue_->try_pop(r)) writer_.write(r);
            return;
        }
        std::this_thread::yield();
    }
}

}  // namespace ts
