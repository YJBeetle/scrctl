#include "app/TextInputQueue.h"

#include <string_view>
#include <utility>

namespace scrctl::app {
namespace {

bool valid_utf8(std::string_view text) noexcept {
    size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned remaining;
        uint32_t value;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1; value = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2; value = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3; value = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > text.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

} // namespace

void TextInputQueue::prepare_epoch(uint64_t epoch) noexcept {
    if (epoch_ && *epoch_ != epoch) clear();
    epoch_ = epoch;
}

TextInputQueue::PushResult TextInputQueue::reject(PushResult result) noexcept {
    clear();
    last_result_ = result;
    return result;
}

TextInputQueue::PushResult TextInputQueue::push_text(std::string text, uint64_t epoch) {
    prepare_epoch(epoch);
    if (text.empty()) return reject(PushResult::EmptyText);
    if (text.find('\0') != std::string::npos) return reject(PushResult::EmbeddedNul);
    if (text.size() > max_text_bytes - text_bytes_) return reject(PushResult::TextLimit);
    if (!valid_utf8(text)) return reject(PushResult::InvalidUtf8);
    if (entries_.size() >= max_entries) return reject(PushResult::EntryLimit);
    const auto bytes = text.size();
    entries_.push_back({Entry::Kind::Text, std::move(text), {}, epoch});
    text_bytes_ += bytes;
    return last_result_ = PushResult::Accepted;
}

TextInputQueue::PushResult TextInputQueue::push_keyboard(KeyboardState::Report report,
                                                       uint64_t epoch) {
    prepare_epoch(epoch);
    if (entries_.size() >= max_entries) return reject(PushResult::EntryLimit);
    entries_.push_back({Entry::Kind::Keyboard, {}, std::move(report), epoch});
    return last_result_ = PushResult::Accepted;
}

const TextInputQueue::Entry *TextInputQueue::front() const noexcept {
    return entries_.empty() ? nullptr : &entries_.front();
}

void TextInputQueue::pop_front() noexcept {
    if (entries_.empty()) return;
    text_bytes_ -= entries_.front().text.size();
    entries_.pop_front();
}

void TextInputQueue::clear() noexcept {
    entries_.clear();
    epoch_.reset();
    text_bytes_ = 0;
    last_result_ = PushResult::Accepted;
}

} // namespace scrctl::app
