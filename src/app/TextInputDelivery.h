#pragma once

#include "app/ClipboardPasteJob.h"
#include "app/TextInputQueue.h"
#include <chrono>

namespace scrctl::app {

/// Main-thread coordinator: clipboard preparation and HID reports share one
/// order. Neither later edits nor a busy worker may overtake committed text.
class TextInputDelivery {
public:
    using Clock = std::chrono::steady_clock;
    using Send = std::function<bool(const KeyboardState::Report &, std::string &)>;
    TextInputDelivery(ClipboardPasteJob &job, Send send);
    bool text(std::string value, uint64_t epoch, std::string &error);
    bool keyboard(KeyboardState::Report value, uint64_t epoch, std::string &error);
    bool service(uint64_t epoch, bool ready, std::string &error, Clock::time_point now = Clock::now());
    /// Cancel queued/in-flight input and immediately release actually delivered
    /// keys. The owner must still shutdown the borrowed job before its Device.
    void cancel();
private:
    void prepare_epoch(uint64_t epoch);
    bool accepted(TextInputQueue::PushResult status, std::string &error);
    bool fail(std::string &error);
    bool send(const KeyboardState::Report &report, std::string &error);
    ClipboardPasteJob &job_;
    Send send_;
    TextInputQueue queue_;
    KeyboardState::Report held_;
    std::string pending_error_;
    uint64_t next_id_ = 0, active_id_ = 0, active_epoch_ = 0;
    std::optional<Clock::time_point> started_;
};
} // namespace scrctl::app
