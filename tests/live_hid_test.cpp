#include "app/DeviceConnection.h"
#include "app/LiveSource.h"
#include "hid/Hid.h"
#include "i18n/Translation.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Exercise the real LiveSource input ownership, failure latch and cleanup.
// Replace only its existing public device-open and HID connection/report boundary;
// no fake RSD server, private accessor, or production test hook is involved.
namespace {
using Source = scrctl::app::LiveSource;
using namespace scrctl::hid::button;
struct Event {
    enum Kind { TouchDown, TouchUp, KeysDown, KeysUp, ButtonDown, ButtonUp } kind;
    uint16_t page = 0, code = 0;
    bool operator==(const Event&) const = default;
};
struct Script {
    unsigned devices = 0, universal = 0, indigo = 0;
    bool universal_ok = true, indigo_ok = true;
    std::vector<Event> events;
    std::map<std::size_t, std::string> failures;
    void reset() { *this = {}; }
    bool send(Event event, std::string& error) {
        events.push_back(event); // A failed operation may have partially arrived.
        const auto failure = failures.find(events.size());
        if (failure != failures.end()) { error = failure->second; return false; }
        error.clear(); return true;
    }
} script;
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void start(Source& source) {
    Source::Options options;
    options.want_video = options.want_audio = false;
    std::string error;
    check(source.start(options, error) && !source.has_video() && !source.has_audio(),
          "actual control-only LiveSource starts without media or a desktop window");
}
void locked(Source& source, const std::string& expected) {
    const auto count = script.events.size();
    const auto universal = script.universal, indigo = script.indigo;
    std::string error;
    check(!source.button_state(kUsagePageConsumer, kHome, true, error) && error == expected,
          "subsequent button DOWN preserves the shared first error");
    check(!source.control(.5, .5, true, error) && error == expected,
          "subsequent touch preserves the shared first error");
    check(!source.keyboard_state({4, 225}, error) && error == expected,
          "subsequent keyboard report preserves the shared first error");
    check(!source.type_text("a", 0, error) && error == expected,
          "diagnostic typing preserves the shared first error");
    check(!source.button(kUsagePageConsumer, kHome, error) && error == expected,
          "diagnostic pulse preserves the shared first error");
    error = "stale";
    check(source.button_state(kUsagePageConsumer, kHome, false, error) && error.empty(),
          "UP after completed failure cleanup is a successful empty operation");
    check(script.events.size() == count && script.universal == universal && script.indigo == indigo,
          "shared failure neither reconnects nor repeats consumed release attempts");
}
void before_start() {
    script.reset(); Source source; std::string error = "stale";
    check(source.button_state(kUsagePageConsumer, kHome, false, error) && error.empty(),
          "unknown UP before start does not lazily open a service");
    check(!source.button_state(kUsagePageConsumer, kHome, true, error) &&
              error == "Device input is unavailable before a session is started",
          "button DOWN before start reports an error instead of dereferencing Device");
    locked(source, error);
    check(script.devices == 0 && script.universal == 0 && script.indigo == 0,
          "pre-start input failure does not open a device or either service");
}
void normal_and_repeat() {
    script.reset();
    {
        Source source; start(source); std::string error;
        check(source.button_state(kUsagePageConsumer, kVolumeUp, true, error), "volume DOWN succeeds");
        check(source.button_state(kUsagePageConsumer, kVolumeUp, true, error), "volume repeat DOWN succeeds");
        check(source.button_state(kUsagePageConsumer, kVolumeUp, false, error), "volume final UP succeeds");
        check(source.button_state(kUsagePageConsumer, kVolumeUp, false, error), "repeated UP is empty");
        check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 233},
                  {Event::ButtonDown, 12, 233}, {Event::ButtonUp, 12, 233}},
              "repeat sends each DOWN and only the final UP");
        check(script.indigo == 1 && script.universal == 0,
              "buttons open indigo once without opening the universal HID service");
        check(source.finish_recording(error), "normal final cleanup succeeds");
    }
    check(script.events.size() == 3, "normal UP is not repeated by finish or destruction");
    script.reset();
    {
        Source source; start(source); std::string error;
        const auto begin = std::chrono::steady_clock::now();
        check(source.button(kUsagePageConsumer, kHome, error), "diagnostic short pulse uses the same state path");
        check(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(90),
              "diagnostic short pulse retains its 90ms hold");
    }
    check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 64}, {Event::ButtonUp, 12, 64}},
          "successful diagnostic pulse produces exactly DOWN and UP");
}
void release_every_input() {
    script.reset();
    {
        Source source; start(source); std::string error;
        check(source.control(.25, .75, true, error), "touch held before cleanup");
        check(source.keyboard_state({4, 225}, error), "modifier and key held before cleanup");
        check(source.button_state(12, 233, true, error), "first Consumer held before cleanup");
        check(source.button_state(12, 234, true, error), "second Consumer held before cleanup");
        check(source.button_state(1, 233, true, error), "different page with same code held independently");
        check(source.button_state(12, 233, true, error), "repeat adds no second cleanup owner");
        check(source.finish_recording(error), "final cleanup releases all held input kinds");
        check(script.events == std::vector<Event>{{Event::TouchDown}, {Event::KeysDown},
                  {Event::ButtonDown, 12, 233}, {Event::ButtonDown, 12, 234}, {Event::ButtonDown, 1, 233},
                  {Event::ButtonDown, 12, 233}, {Event::TouchUp}, {Event::KeysUp},
                  {Event::ButtonUp, 12, 233}, {Event::ButtonUp, 12, 234}, {Event::ButtonUp, 1, 233}},
              "cleanup releases touch, keyboard and each page/code once despite repeated DOWN");
        check(source.finish_recording(error), "second final cleanup succeeds");
    }
    check(script.events.size() == 11 && script.universal == 1 && script.indigo == 1,
          "finish and destructor are idempotent and use only established connections");
    script.reset();
    {
        Source source; start(source); std::string error;
        source.button_state(12, 64, true, error);
    }
    check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 64}, {Event::ButtonUp, 12, 64}} &&
              script.universal == 0,
          "destructor releases an indigo-only held button without a universal connection");
}
void send_failures() {
    script.reset();
    {
        Source source; start(source); std::string error;
        script.failures[1] = "partial DOWN";
        script.failures[2] = "cleanup UP";
        check(!source.button_state(12, 64, true, error) && error == "partial DOWN",
              "failed first DOWN retains the original send error");
        check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 64}, {Event::ButtonUp, 12, 64}},
              "possibly delivered first DOWN gets a same-connection release even when that also fails");
        locked(source, "partial DOWN");
    }
    check(script.events.size() == 2, "destructor does not retry already consumed failed cleanup");

    script.reset();
    {
        Source source; start(source); std::string error;
        source.button_state(12, 233, true, error);
        source.button_state(12, 234, true, error);
        script.failures[3] = "original UP";
        script.failures[4] = "retry UP";
        check(!source.button_state(12, 233, false, error) && error == "original UP",
              "failed explicit UP remains owned until its one cleanup retry");
        check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 233}, {Event::ButtonDown, 12, 234},
                  {Event::ButtonUp, 12, 233}, {Event::ButtonUp, 12, 233}, {Event::ButtonUp, 12, 234}},
              "UP failure retries that button once and still releases the other held button");
        locked(source, "original UP");
    }

    script.reset();
    {
        Source source; start(source); std::string error;
        source.button_state(12, 64, true, error);
        source.keyboard_state({225}, error);
        script.failures[3] = "partial touch";
        script.failures[4] = "touch cleanup";
        script.failures[5] = "keyboard cleanup";
        check(!source.control(.2, .3, true, error) && error == "partial touch",
              "touch send failure preserves its first error while other input kinds are held");
        check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 64}, {Event::KeysDown},
                  {Event::TouchDown}, {Event::TouchUp}, {Event::KeysUp}, {Event::ButtonUp, 12, 64}},
              "failure cleanup attempts all three kinds even after earlier cleanup failures");
        locked(source, "partial touch");
    }
}
void service_and_cleanup_failures() {
    script.reset();
    {
        Source source; start(source); std::string error;
        source.button_state(12, 64, true, error);
        script.universal_ok = false;
        check(!source.keyboard_state({4}, error) && error == "universal open failure",
              "universal service failure locks the same shared input state");
        check(script.events == std::vector<Event>{{Event::ButtonDown, 12, 64}, {Event::ButtonUp, 12, 64}},
              "universal connection failure still releases an indigo-only held button");
        locked(source, "universal open failure");
    }
    script.reset();
    {
        Source source; start(source); std::string error;
        source.control(.1, .2, true, error); source.keyboard_state({4}, error);
        script.indigo_ok = false;
        check(!source.button_state(12, 64, true, error) && error == "indigo open failure",
              "indigo service failure does not send a DOWN");
        check(script.events == std::vector<Event>{{Event::TouchDown}, {Event::KeysDown},
                  {Event::TouchUp}, {Event::KeysUp}},
              "indigo connection failure releases the existing universal input state");
        locked(source, "indigo open failure");
    }
    script.reset();
    {
        Source source; start(source); std::string error;
        source.control(.1, .2, true, error); source.keyboard_state({4}, error);
        source.button_state(12, 64, true, error);
        script.failures[4] = "first cleanup"; script.failures[5] = "later cleanup";
        check(source.finish_recording(error), "input cleanup does not invent a recording error");
        check(script.events.size() == 6 && script.events.back() == Event{Event::ButtonUp, 12, 64},
              "final cleanup continues to buttons after touch and keyboard release failure");
        locked(source, "first cleanup");
    }
    check(script.events.size() == 6, "failed final cleanup is not resent during destruction");
}
} // namespace

namespace scrctl::app {
std::optional<remote::Device> open_device(const std::string&, const std::string&,
                                         std::string& error, uint16_t) {
    ++script.devices; error.clear(); return remote::Device{};
}
} // namespace scrctl::app

namespace scrctl::hid {
uint64_t report_timestamp() { return 0; }
std::vector<uint8_t> keyboard_report(const std::vector<uint16_t>& usages, uint64_t) {
    return {static_cast<uint8_t>(!usages.empty())};
}
std::unique_ptr<Service> Service::open(remote::Device&, std::string& error, bool) {
    ++script.universal;
    if (!script.universal_ok) { error = "universal open failure"; return nullptr; }
    error.clear(); return std::unique_ptr<Service>(new Service(std::make_unique<remote::ServiceConnection>()));
}
bool Service::touch(uint64_t, double, double, bool down, std::string& error) {
    return script.send({down ? Event::TouchDown : Event::TouchUp}, error);
}
bool Service::send_report(uint64_t, std::span<const uint8_t> report, std::string& error, xpc::Value*) {
    return script.send({!report.empty() && report.front() ? Event::KeysDown : Event::KeysUp}, error);
}
bool Service::type_text(const std::string&, int, std::string& error) {
    return script.send({Event::KeysDown}, error) && script.send({Event::KeysUp}, error);
}
std::unique_ptr<Buttons> Buttons::open(remote::Device&, std::string& error, bool) {
    ++script.indigo;
    if (!script.indigo_ok) { error = "indigo open failure"; return nullptr; }
    error.clear(); return std::unique_ptr<Buttons>(new Buttons(std::make_unique<remote::ServiceConnection>()));
}
bool Buttons::down(uint16_t page, uint16_t code, std::string& error) {
    return script.send({Event::ButtonDown, page, code}, error);
}
bool Buttons::release(uint16_t page, uint16_t code, std::string& error) {
    return script.send({Event::ButtonUp, page, code}, error);
}
} // namespace scrctl::hid

int main() {
    scrctl::i18n::initialize("en");
    before_start(); normal_and_repeat(); release_every_input(); send_failures(); service_and_cleanup_failures();
    std::printf("live_hid: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
