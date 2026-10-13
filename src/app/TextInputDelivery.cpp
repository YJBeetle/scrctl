#include "app/TextInputDelivery.h"
#include "hid/Hid.h"
#include "i18n/Translation.h"
#include <utility>

namespace scrctl::app {
TextInputDelivery::TextInputDelivery(ClipboardPasteJob &job, Send send)
    : job_(job), send_(std::move(send)) {}

void TextInputDelivery::cancel() {
    job_.cancel();
    queue_.clear();
    started_.reset();
    active_id_ = 0;
    if (!held_.empty()) {
        std::string ignored;
        if (!send_({}, ignored))
            pending_error_ = ignored.empty() ? SCRCTL_TR("Failed to release text input keys") : ignored;
        held_.clear();
    }
}
bool TextInputDelivery::fail(std::string &) { cancel(); return false; }
bool TextInputDelivery::accepted(TextInputQueue::PushResult status, std::string &error) {
    if (status == TextInputQueue::PushResult::Accepted) return true;
    error = SCRCTL_TR("Text input queue rejected a submission; queued input cancelled");
    return fail(error);
}
void TextInputDelivery::prepare_epoch(uint64_t epoch) {
    if ((queue_.front() && queue_.front()->epoch != epoch) ||
        (started_ && active_epoch_ != epoch)) cancel();
}
bool TextInputDelivery::text(std::string value, uint64_t epoch, std::string &error) {
    prepare_epoch(epoch);
    return accepted(queue_.push_text(std::move(value), epoch), error);
}
bool TextInputDelivery::keyboard(KeyboardState::Report value, uint64_t epoch, std::string &error) {
    prepare_epoch(epoch);
    return accepted(queue_.push_keyboard(std::move(value), epoch), error);
}
bool TextInputDelivery::send(const KeyboardState::Report &report, std::string &error) {
    // Record even attempted DOWN: a failed local write can still reach the peer.
    held_ = report;
    if (!send_(report, error)) return fail(error);
    return true;
}
bool TextInputDelivery::service(uint64_t epoch, bool ready, std::string &error, Clock::time_point now) {
    prepare_epoch(epoch);
    if (!ready) cancel();
    if (!pending_error_.empty()) {
        error = std::exchange(pending_error_, {});
        return false;
    }
    if (started_ && now - *started_ >= std::chrono::seconds(5)) {
        error = SCRCTL_TR("Text input clipboard preparation timed out");
        return fail(error);
    }
    if (auto result = job_.poll()) {
        if (active_id_ && result->id == active_id_ && result->generation == active_epoch_) {
            started_.reset();
            active_id_ = 0;
            if (result->cancelled || !result->success) {
                error = result->error.empty() ? SCRCTL_TR("Text input clipboard preparation failed") : result->error;
                return fail(error);
            }
            if (result->generation != epoch || !ready || !queue_.front() ||
                queue_.front()->kind != TextInputQueue::Entry::Kind::Text) {
                cancel();
                return true;
            }
            const auto restore = held_;
            constexpr uint16_t v = hid::key::kA + ('v' - 'a');
            if (!send({}, error) || !send({hid::key::kGuiLeft}, error) ||
                !send({hid::key::kGuiLeft, v}, error) || !send({}, error) ||
                (!restore.empty() && !send(restore, error))) return false;
            queue_.pop_front();
        }
    }
    if (!ready || job_.busy() || started_) return true;
    while (const auto *entry = queue_.front()) {
        if (entry->epoch != epoch) { cancel(); return true; }
        if (entry->kind == TextInputQueue::Entry::Kind::Keyboard) {
            const auto report = entry->keyboard;
            queue_.pop_front();
            if (!send(report, error)) return false;
            continue;
        }
        const auto id = ++next_id_;
        if (job_.start(id, epoch, entry->text) != ClipboardPasteJob::StartStatus::Started) {
            error = SCRCTL_TR("Could not start text input clipboard preparation");
            return fail(error);
        }
        active_id_ = id;
        active_epoch_ = epoch;
        started_ = now;
        break;
    }
    return true;
}
} // namespace scrctl::app
