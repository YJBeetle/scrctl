#pragma once

#include "app/KeyboardState.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace scrctl::app {

/// 单线程拥有的输入交付队列；Text 与完整键盘状态严格保持入队顺序。
/// front() 包含在途条目，交付完成才 pop_front()，因此在途文本也占预算。
class TextInputQueue {
public:
    static constexpr size_t max_entries = 256;
    static constexpr size_t max_text_bytes = 1024 * 1024;

    struct Entry {
        enum class Kind { Text, Keyboard };
        Kind kind;
        std::string text;
        KeyboardState::Report keyboard;
        uint64_t epoch;
    };
    enum class PushResult {
        Accepted, EmptyText, EmbeddedNul, InvalidUtf8, EntryLimit, TextLimit
    };

    /// 任意拒绝均清空全部旧输入，返回具体原因；调用者应取消在途交付、释放设备键并警告。
    /// 不同 epoch 会先清空旧输入。调用者必须过滤过期异步回调，避免其弹出新 epoch 条目。
    PushResult push_text(std::string text, uint64_t epoch);
    PushResult push_keyboard(KeyboardState::Report report, uint64_t epoch);

    const Entry *front() const noexcept;
    void pop_front() noexcept;
    void clear() noexcept;
    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }
    size_t text_bytes() const noexcept { return text_bytes_; }
    PushResult last_result() const noexcept { return last_result_; }

private:
    void prepare_epoch(uint64_t epoch) noexcept;
    PushResult reject(PushResult result) noexcept;

    std::deque<Entry> entries_;
    std::optional<uint64_t> epoch_;
    size_t text_bytes_ = 0;
    PushResult last_result_ = PushResult::Accepted;
};

} // namespace scrctl::app
