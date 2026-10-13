#include "app/TextInputDelivery.h"
#include "hid/Hid.h"
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>

using namespace scrctl::app;
namespace hid = scrctl::hid;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char *what) { if (!value) throw std::runtime_error(what); }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned credits = 0;
    std::vector<std::string> requests;
    ClipboardPasteJob::OperationResult run(const std::string &text, std::stop_token token) {
        std::stop_callback stop(token, [&] { changed.notify_all(); });
        std::unique_lock lock(mutex);
        requests.push_back(text);
        changed.notify_all();
        changed.wait(lock, [&] { return credits || token.stop_requested(); });
        if (token.stop_requested()) return {false, true, {}};
        --credits;
        return {true, false, {}};
    }
    void permit() { std::lock_guard lock(mutex); ++credits; changed.notify_all(); }
    void wait(unsigned n) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, [&] { return requests.size() >= n; }), "worker starts requested text");
    }
};
void drain(TextInputDelivery &delivery, ClipboardPasteJob &job, uint64_t epoch,
           const std::function<bool()> &done) {
    const auto limit = TextInputDelivery::Clock::now() + 2s;
    std::string error;
    (void)job;
    do {
        require(delivery.service(epoch, true, error), "ordered delivery services successfully");
        if (done()) return;
        std::this_thread::sleep_for(1ms);
    } while (TextInputDelivery::Clock::now() < limit);
    throw std::runtime_error("delivery completion deadline");
}
void ordered_edits() {
    Gate gate;
    ClipboardPasteJob job([&](auto &text, auto cancel) { return gate.run(text, cancel); });
    std::vector<KeyboardState::Report> sent;
    TextInputDelivery delivery(job, [&](auto &report, std::string &) { sent.push_back(report); return true; });
    std::string error;
    require(delivery.text("中文🙂", 1, error) && delivery.keyboard({42}, 1, error) &&
            delivery.keyboard({}, 1, error) && delivery.text("B", 1, error) &&
            delivery.keyboard({40}, 1, error) && delivery.keyboard({}, 1, error), "enqueue interleaved text/edits");
    require(delivery.service(1, true, error), "start first clipboard preparation");
    gate.wait(1);
    require(sent.empty(), "backspace cannot overtake busy text");
    gate.permit();
    drain(delivery, job, 1, [&] { std::lock_guard lock(gate.mutex); return gate.requests.size() == 2; });
    constexpr uint16_t v = hid::key::kA + ('v' - 'a');
    require(sent == std::vector<KeyboardState::Report>{{}, {hid::key::kGuiLeft},
        {hid::key::kGuiLeft, v}, {}, {42}, {}}, "first paste precedes backspace, second text remains busy");
    gate.permit();
    drain(delivery, job, 1, [&] { return !job.busy() && sent.size() == 12; });
    require(sent[10] == KeyboardState::Report{40} && sent[11].empty(), "enter follows both committed texts");
    require(gate.requests == std::vector<std::string>{"中文🙂", "B"}, "Unicode submissions preserve bytes and order");
}
void cancellation_and_timeout() {
    Gate gate;
    ClipboardPasteJob job([&](auto &text, auto cancel) { return gate.run(text, cancel); });
    std::vector<KeyboardState::Report> sent;
    TextInputDelivery delivery(job, [&](auto &report, std::string &) { sent.push_back(report); return true; });
    std::string error;
    require(delivery.keyboard({uint16_t(0xE0)}, 1, error) && delivery.text("old", 1, error) &&
            delivery.keyboard({42}, 1, error), "enqueue held modifier and text");
    const auto at = TextInputDelivery::Clock::now();
    require(delivery.service(1, true, error, at), "start cancellable text");
    gate.wait(1);
    require(delivery.service(2, false, error), "focus/context change cancels old text");
    require(sent.size() == 2 && sent.front() == KeyboardState::Report{uint16_t(0xE0)} &&
            sent.back().empty(), "context change immediately releases actual modifier, no edit/paste");
    drain(delivery, job, 2, [&] { return !job.busy(); });
    require(delivery.text("new", 2, error) && delivery.service(2, true, error, at), "new context can accept text");
    gate.wait(2);
    require(!delivery.service(2, true, error, at + 5s) && !error.empty(), "clipboard timeout cancels text rather than overtaking it");
    job.shutdown();
    require(sent.size() == 2, "cancelled preparation never injects paste");
}
void modifier_restore_and_failure() {
    ClipboardPasteJob job([](auto &, auto) { return ClipboardPasteJob::OperationResult{true, false, {}}; });
    std::vector<KeyboardState::Report> sent;
    TextInputDelivery delivery(job, [&](auto &report, std::string &) { sent.push_back(report); return true; });
    std::string error;
    require(delivery.keyboard({0xE1}, 1, error) && delivery.text("A", 1, error) &&
            delivery.keyboard({}, 1, error), "queue text while device modifier is held");
    drain(delivery, job, 1, [&] { return sent.size() == 7 && !job.busy(); });
    require(sent[0] == KeyboardState::Report{0xE1} && sent[1].empty() &&
            sent[2] == KeyboardState::Report{hid::key::kGuiLeft} &&
            sent[5] == KeyboardState::Report{0xE1} && sent[6].empty(),
            "paste temporarily clears actual modifiers and restores before queued UP");

    ClipboardPasteJob broken_job([](auto &, auto) { return ClipboardPasteJob::OperationResult{true, false, {}}; });
    sent.clear();
    constexpr uint16_t v = hid::key::kA + ('v' - 'a');
    TextInputDelivery broken(broken_job, [&](auto &report, std::string &why) {
        sent.push_back(report);
        if (report == KeyboardState::Report{hid::key::kGuiLeft, v}) { why = "send failed"; return false; }
        return true;
    });
    require(broken.text("A", 1, error) && broken.keyboard({40}, 1, error), "queue text before HID failure");
    bool ok = true;
    const auto limit = TextInputDelivery::Clock::now() + 2s;
    while (ok && TextInputDelivery::Clock::now() < limit) {
        ok = broken.service(1, true, error);
        std::this_thread::sleep_for(1ms);
    }
    require(!ok && error == "send failed" && sent.back().empty(), "HID failure releases attempted keys");
    require(std::find(sent.begin(), sent.end(), KeyboardState::Report{40}) == sent.end(),
            "HID failure cannot let later enter overtake text");
}
void prepare_failure() {
    ClipboardPasteJob job([](auto &, auto) { return ClipboardPasteJob::OperationResult{false, false, "denied"}; });
    unsigned sent = 0;
    TextInputDelivery delivery(job, [&](auto &, std::string &) { ++sent; return true; });
    std::string error;
    require(delivery.text("text", 1, error) && delivery.keyboard({40}, 1, error), "queue failing paste and later enter");
    require(delivery.service(1, true, error), "failed job starts asynchronously");
    const auto limit = TextInputDelivery::Clock::now() + 2s;
    bool ok = true;
    while (ok && TextInputDelivery::Clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
        ok = delivery.service(1, true, error);
    }
    require(!ok && error == "denied" && !sent, "preparation failure blocks paste and following enter");
}
}
int main() {
    try { ordered_edits(); cancellation_and_timeout(); modifier_restore_and_failure(); prepare_failure(); }
    catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
    return 0;
}
