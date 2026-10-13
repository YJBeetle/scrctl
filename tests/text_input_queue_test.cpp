#include "app/TextInputQueue.h"

#include <cstdio>
#include <string>

namespace {
using Queue = scrctl::app::TextInputQueue;
using Result = Queue::PushResult;
using Kind = Queue::Entry::Kind;
int failures = 0;

void check(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void ordered_delivery() {
    Queue queue;
    queue.push_keyboard({225}, 7);
    queue.push_text("中文✓🙂", 7);
    queue.push_keyboard({}, 7);
    queue.push_text("second", 7);
    check(queue.front() && queue.front()->kind == Kind::Keyboard &&
          queue.front()->keyboard == scrctl::app::KeyboardState::Report{225},
          "modifier down precedes text");
    queue.pop_front();
    check(queue.front() && queue.front()->text == "中文✓🙂" &&
          queue.front()->epoch == 7, "UTF-8 commit arrives unchanged");
    const auto bytes = queue.text_bytes();
    check(queue.front() && queue.text_bytes() == bytes && queue.size() == 3,
          "reading an in-flight front does not release its budget");
    queue.pop_front();
    check(queue.front() && queue.front()->kind == Kind::Keyboard &&
          queue.front()->keyboard.empty(), "modifier up cannot overtake text");
    queue.pop_front();
    check(queue.front() && queue.front()->text == "second", "next commit follows up");
    queue.pop_front();
    queue.pop_front();
    check(queue.empty() && queue.text_bytes() == 0 && !queue.front(), "drained queue");
}

void capacity_and_recovery() {
    Queue queue;
    for (size_t i = 0; i < Queue::max_entries; ++i)
        check(queue.push_keyboard({4}, 1) == Result::Accepted, "entry boundary accepted");
    check(queue.size() == Queue::max_entries, "all queued and in-flight entries count");
    check(queue.push_keyboard({}, 1) == Result::EntryLimit && queue.empty() &&
          queue.last_result() == Result::EntryLimit,
          "capacity refusal clears pending down states instead of reordering up");
    check(queue.push_text("recovered", 1) == Result::Accepted, "new input recovers after refusal");
    queue.clear();
    const auto half = Queue::max_text_bytes / 2;
    queue.push_text(std::string(half, 'a'), 2);
    check(queue.push_text(std::string(half, 'b'), 2) == Result::Accepted &&
          queue.text_bytes() == Queue::max_text_bytes, "cumulative byte boundary accepted");
    check(queue.push_text("c", 2) == Result::TextLimit && queue.empty() &&
          queue.text_bytes() == 0, "byte overflow explicitly clears all pending text");
    queue.push_text(std::string(half, 'a'), 2);
    queue.push_text(std::string(half, 'b'), 2);
    queue.pop_front();
    check(queue.push_text(std::string(half, 'c'), 2) == Result::Accepted,
          "completed delivery releases byte capacity");
    queue.clear();
    check(queue.push_text(std::string(Queue::max_text_bytes + 1, 'a'), 2) == Result::TextLimit,
          "single oversized commit is refused");
}

void validation_and_epoch() {
    Queue queue;
    const std::string invalid[] = {
        std::string("\x80", 1), std::string("\xc0\xaf", 2),
        std::string("\xe0\x80\x80", 3), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xf0\x9f\x99", 3),
        std::string("\xe4x\xad", 3)
    };
    for (const auto &text : invalid) {
        queue.push_keyboard({4}, 3);
        check(queue.push_text(text, 3) == Result::InvalidUtf8 && queue.empty(),
              "invalid scalar or partial UTF-8 refuses and clears the input sequence");
    }
    check(queue.push_text({}, 3) == Result::EmptyText, "empty commit is refused");
    check(queue.push_text(std::string("a\0b", 3), 3) == Result::EmbeddedNul,
          "embedded NUL cannot truncate delivered text");
    check(queue.push_text(std::string("\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80"
                                     "\xed\x9f\xbf\xee\x80\x80\xf4\x8f\xbf\xbf"), 3)
          == Result::Accepted, "legal UTF-8 scalar boundaries accepted");
    check(queue.push_text("new epoch", 4) == Result::Accepted && queue.size() == 1 &&
          queue.front()->epoch == 4 && queue.front()->text == "new epoch",
          "new epoch discards old pending and in-flight entries");
    queue.clear();
    check(queue.empty() && queue.text_bytes() == 0 &&
          queue.push_keyboard({}, 5) == Result::Accepted,
          "explicit cancellation clears queue and permits a fresh epoch");
}
} // namespace

int main() {
    ordered_delivery();
    capacity_and_recovery();
    validation_and_epoch();
    return failures ? 1 : 0;
}
