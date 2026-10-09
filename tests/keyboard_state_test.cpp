#include "app/KeyboardState.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace {

using scrctl::app::KeyboardState;
using Report = KeyboardState::Report;
using Reports = KeyboardState::Reports;

int checks = 0;
int failures = 0;

void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void expect(const Reports &actual, const Reports &expected, const char *message) {
    check(actual == expected, message);
    for (const auto &report : actual)
        check(std::is_sorted(report.begin(), report.end()) &&
                  std::adjacent_find(report.begin(), report.end()) == report.end() &&
                  std::all_of(report.begin(), report.end(), [](uint16_t usage) {
                      return usage >= 4 && usage < 240;
                  }),
              "reports are ordered, unique and within the keyboard bitmap");
}

void basic_keys() {
    KeyboardState state;
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "A down");
    expect(state.key_down(SDL_SCANCODE_B, KMOD_NONE, false), {{4, 5}}, "B preserves held A");
    check(state.held() == Report{4, 5}, "held returns the complete state");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{5}}, "A up preserves B");
    expect(state.key_up(SDL_SCANCODE_B, KMOD_NONE), {{}}, "last key up sends empty state");
    expect(state.key_up(SDL_SCANCODE_B, KMOD_RSHIFT), {}, "orphan key up cannot introduce a modifier");
    expect(state.release_all(), {}, "already empty release does not send another empty report");

    expect(state.key_down(SDL_SCANCODE_Q, KMOD_NONE, false), {{20}}, "plain Q belongs to the device");
    expect(state.key_up(SDL_SCANCODE_Q, KMOD_NONE), {{}}, "plain Q releases");
    expect(state.key_down(SDL_SCANCODE_ESCAPE, KMOD_NONE, false), {{41}}, "plain Escape belongs to the device");
    expect(state.key_up(SDL_SCANCODE_ESCAPE, KMOD_NONE), {{}}, "plain Escape releases");
}

void modifier_prefix_preserves_keys() {
    KeyboardState state;
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "start holding A");
    expect(state.key_down(SDL_SCANCODE_B, KMOD_LSHIFT, false),
           {{4, 225}, {4, 5, 225}}, "new Shift prefix preserves A before adding B");
    expect(state.key_up(SDL_SCANCODE_B, KMOD_LSHIFT), {{4, 225}}, "B up keeps A and Shift");
    expect(state.key_up(SDL_SCANCODE_LSHIFT, KMOD_LSHIFT), {{4}}, "Shift up overrides a stale own modifier bit");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "remaining A releases");

    expect(state.key_down(SDL_SCANCODE_1, KMOD_RSHIFT, false),
           {{229}, {30, 229}}, "Shift punctuation sends modifier before the physical key");
    expect(state.key_up(SDL_SCANCODE_1, KMOD_RSHIFT), {{229}}, "punctuation up keeps right Shift");
    expect(state.key_up(SDL_SCANCODE_RSHIFT, KMOD_NONE), {{}}, "inferred right Shift has a working key up");

    expect(state.key_down(SDL_SCANCODE_A, KMOD_LSHIFT, false), {{225}, {4, 225}}, "infer Shift on new A");
    expect(state.key_down(SDL_SCANCODE_B, KMOD_NONE, false),
           {{4}, {4, 5}}, "modifier removal precedes a new ordinary key without releasing A");
    expect(state.key_down(SDL_SCANCODE_LSHIFT, KMOD_NONE, false), {{4, 5, 225}},
           "a real Shift down works after a prior snapshot released inferred Shift");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_LSHIFT), {{5, 225}}, "A releases after modifier re-press");
    expect(state.release_all(), {{}}, "cleanup releases B and Shift");

    expect(state.key_down(SDL_SCANCODE_A, KMOD_RGUI, false), {{231}, {4, 231}}, "infer right GUI before A");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "A up snapshot also releases inferred right GUI");
    expect(state.key_down(SDL_SCANCODE_RGUI, KMOD_NONE, false), {{231}},
           "a real right GUI down works after an ordinary key up released its snapshot");
    expect(state.key_up(SDL_SCANCODE_RGUI, KMOD_NONE), {{}}, "re-pressed right GUI releases normally");
}

void explicit_modifiers() {
    // 独立给出 USB usages，不从被测映射取期望值。
    struct Case { SDL_Scancode scancode; uint16_t mask; uint16_t usage; };
    const std::array<Case, 8> cases{{
        {SDL_SCANCODE_LCTRL, KMOD_LCTRL, 224},
        {SDL_SCANCODE_LSHIFT, KMOD_LSHIFT, 225},
        {SDL_SCANCODE_LALT, KMOD_LALT, 226},
        {SDL_SCANCODE_LGUI, KMOD_LGUI, 227},
        {SDL_SCANCODE_RCTRL, KMOD_RCTRL, 228},
        {SDL_SCANCODE_RSHIFT, KMOD_RSHIFT, 229},
        {SDL_SCANCODE_RALT, KMOD_RALT, 230},
        {SDL_SCANCODE_RGUI, KMOD_RGUI, 231},
    }};
    for (const auto &c : cases) {
        KeyboardState state(0);
        expect(state.key_down(c.scancode, KMOD_NONE, false), {{c.usage}},
               "explicit modifier down works before SDL adds its snapshot bit");
        expect(state.key_down(c.scancode, c.mask, false), {}, "duplicate modifier down is silent");
        expect(state.key_down(SDL_SCANCODE_A, c.mask, false), {{4, c.usage}},
               "known modifier needs no extra prefix on A");
        expect(state.key_up(c.scancode, c.mask), {{4}}, "explicit modifier up clears a stale own snapshot bit");
        expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "A up after modifier up");
    }

    KeyboardState state(0);
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "A before explicit Control");
    expect(state.key_down(SDL_SCANCODE_LCTRL, KMOD_NONE, false), {{4, 224}},
           "explicit Control down does not release an ordinary key");
    expect(state.key_down(SDL_SCANCODE_C, KMOD_LCTRL, false), {{4, 6, 224}}, "Control+C keeps A");
    expect(state.key_up(SDL_SCANCODE_C, KMOD_LCTRL), {{4, 224}}, "C up keeps Control and A");
    expect(state.key_up(SDL_SCANCODE_LCTRL, KMOD_NONE), {{4}}, "Control up keeps A");
    expect(state.release_all(), {{}}, "release remaining A");
}

void shortcuts_keep_ownership() {
    for (const auto shortcut : {std::pair{SDL_SCANCODE_LALT, uint16_t(KMOD_LALT)},
                                std::pair{SDL_SCANCODE_LGUI, uint16_t(KMOD_LGUI)}}) {
        KeyboardState state;
        expect(state.key_down(shortcut.first, KMOD_NONE, false), {}, "default shortcut modifier itself is local");
        expect(state.key_down(SDL_SCANCODE_F, shortcut.second, false, true), {}, "consumed fullscreen key is local");
        expect(state.key_up(shortcut.first, shortcut.second), {}, "local MOD up emits no device report");
        expect(state.key_up(SDL_SCANCODE_F, KMOD_NONE), {}, "consumed F up remains local after MOD up");
        expect(state.release_all(), {}, "local-only held state needs no device empty report");
        expect(state.key_down(SDL_SCANCODE_F, KMOD_NONE, false), {{9}}, "fresh ordinary F is accepted after local F up");
        expect(state.release_all(), {{}}, "release ordinary F");

        expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "device A first");
        expect(state.key_down(shortcut.first, shortcut.second, false), {}, "pressing MOD does not seize held A");
        expect(state.key_down(SDL_SCANCODE_A, shortcut.second, false, true), {}, "later local duplicate cannot change A ownership");
        expect(state.key_up(SDL_SCANCODE_A, shortcut.second), {{}}, "held device A still releases while MOD is pressed");
        expect(state.key_up(shortcut.first, KMOD_NONE), {}, "then release MOD locally");

        expect(state.key_down(shortcut.first, shortcut.second, false), {}, "MOD pressed for reverse release order");
        expect(state.key_down(SDL_SCANCODE_Q, shortcut.second, false, true), {}, "quit chord Q consumed locally");
        expect(state.key_up(SDL_SCANCODE_Q, shortcut.second), {}, "Q up while MOD held is local");
        expect(state.key_up(shortcut.first, KMOD_NONE), {}, "MOD up after Q remains local");
    }

    KeyboardState state;
    expect(state.key_down(SDL_SCANCODE_F11, KMOD_NONE, false, true), {}, "caller can consume F11 without a modifier");
    expect(state.key_down(SDL_SCANCODE_F11, KMOD_NONE, false), {}, "duplicate F11 cannot switch local owner to device");
    expect(state.key_up(SDL_SCANCODE_F11, KMOD_RSHIFT), {}, "consumed F11 up cannot introduce right Shift");
    expect(state.key_down(SDL_SCANCODE_F11, KMOD_NONE, false), {{68}}, "F11 ownership decision remains at the caller");
    expect(state.release_all(), {{}}, "release forwarded F11");
}

void shortcut_modifier_filtering() {
    KeyboardState state;
    expect(state.key_down(SDL_SCANCODE_A, KMOD_LALT | KMOD_LGUI | KMOD_RALT | KMOD_RGUI, false),
           {{230, 231}, {4, 230, 231}}, "selected left modifiers are excluded; right modifiers remain device keys");
    expect(state.release_all(), {{}}, "release A and right modifiers");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_LCTRL | KMOD_RALT | KMOD_MODE, false),
           {{224, 230}, {4, 224, 230}}, "AltGr retains right Alt and Control; MODE is not a held usage");
    expect(state.release_all(), {{}}, "release AltGr state");

    KeyboardState custom(KMOD_LCTRL);
    expect(custom.key_down(SDL_SCANCODE_LCTRL, KMOD_LCTRL, false), {}, "custom left Control is local");
    expect(custom.key_down(SDL_SCANCODE_A, KMOD_LCTRL | KMOD_LALT | KMOD_RCTRL, false),
           {{226, 228}, {4, 226, 228}}, "custom shortcut replaces defaults without hiding right Control");
    expect(custom.release_all(), {{}}, "release custom modifier state");

    KeyboardState consumed_modifier(0);
    expect(consumed_modifier.key_down(SDL_SCANCODE_LSHIFT, KMOD_LSHIFT, false, true), {},
           "caller-owned modifier stays local even when not configured as MOD");
    expect(consumed_modifier.key_down(SDL_SCANCODE_A, KMOD_LSHIFT, false), {{4}},
           "a snapshot cannot reintroduce a locally owned modifier");
    expect(consumed_modifier.key_up(SDL_SCANCODE_LSHIFT, KMOD_NONE), {}, "consumed modifier up does not alter device A");
    expect(consumed_modifier.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "device A releases normally");
}

void repeats_release_and_fast_events() {
    KeyboardState state;
    expect(state.key_down(SDL_SCANCODE_A, KMOD_LSHIFT, true), {}, "repeat without initial down does not synthesize state");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_LSHIFT), {}, "repeat-only key up remains unknown");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "new A down after ignored repeat");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_RSHIFT, true), {}, "repeat cannot change modifier state");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_RSHIFT, false), {}, "duplicate down cannot change modifier state");
    check(state.held() == Report{4}, "repeat keeps the original full state");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "fast down/up preserves both reports");

    expect(state.key_down(SDL_SCANCODE_A, KMOD_RCTRL, false), {{228}, {4, 228}}, "device state before focus cleanup");
    expect(state.key_down(SDL_SCANCODE_F, KMOD_LALT, false, true), {}, "also hold a local key before cleanup");
    expect(state.release_all(), {{}}, "cleanup emits exactly one empty state");
    check(state.held().empty(), "cleanup removes every device key");
    expect(state.release_all(), {}, "cleanup is idempotent");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_RCTRL), {}, "late old-window A up cannot revive a modifier");
    expect(state.key_up(SDL_SCANCODE_F, KMOD_NONE), {}, "late local key up is harmless");
    expect(state.key_down(SDL_SCANCODE_F, KMOD_NONE, false), {{9}}, "cleanup also clears local ownership");
    expect(state.release_all(), {{}}, "release fresh F");
}

void whitelist_and_lock_bits() {
    const std::array<std::pair<SDL_Scancode, uint16_t>, 18> cases{{
        {SDL_SCANCODE_Z, 29}, {SDL_SCANCODE_0, 39}, {SDL_SCANCODE_MINUS, 45},
        {SDL_SCANCODE_LEFTBRACKET, 47}, {SDL_SCANCODE_NONUSHASH, 50},
        {SDL_SCANCODE_SLASH, 56}, {SDL_SCANCODE_F1, 58}, {SDL_SCANCODE_F24, 115},
        {SDL_SCANCODE_HOME, 74}, {SDL_SCANCODE_DELETE, 76}, {SDL_SCANCODE_UP, 82},
        {SDL_SCANCODE_KP_ENTER, 88}, {SDL_SCANCODE_KP_0, 98},
        {SDL_SCANCODE_KP_PERIOD, 99}, {SDL_SCANCODE_NONUSBACKSLASH, 100},
        {SDL_SCANCODE_APPLICATION, 101}, {SDL_SCANCODE_KP_EQUALS, 103},
        {SDL_SCANCODE_KP_COMMA, 133},
    }};
    KeyboardState state;
    for (const auto &c : cases) {
        expect(state.key_down(c.first, KMOD_CAPS | KMOD_NUM | KMOD_SCROLL, false), {{c.second}},
               "whitelisted physical usage ignores lock status bits");
        expect(state.key_up(c.first, KMOD_CAPS | KMOD_NUM | KMOD_SCROLL), {{}}, "whitelisted usage releases without lock modifiers");
    }
    expect(state.key_down(SDL_SCANCODE_CAPSLOCK, KMOD_CAPS, false), {{57}}, "CapsLock physical key is accepted");
    expect(state.key_up(SDL_SCANCODE_CAPSLOCK, KMOD_CAPS), {{}}, "CapsLock state is not a held modifier");
    expect(state.key_down(SDL_SCANCODE_NUMLOCKCLEAR, KMOD_NUM, false), {{83}}, "NumLock physical key is accepted");
    expect(state.key_up(SDL_SCANCODE_NUMLOCKCLEAR, KMOD_NUM), {{}}, "NumLock snapshot does not hold a usage");

    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "hold A while rejecting unsupported inputs");
    for (const auto scancode : {SDL_SCANCODE_UNKNOWN, static_cast<SDL_Scancode>(1),
                               static_cast<SDL_Scancode>(2), static_cast<SDL_Scancode>(3),
                               SDL_SCANCODE_AUDIOPLAY, SDL_SCANCODE_VOLUMEUP,
                               SDL_SCANCODE_POWER, SDL_SCANCODE_MODE,
                               SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_KP_00,
                               static_cast<SDL_Scancode>(SDL_NUM_SCANCODES),
                               static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 1)}) {
        expect(state.key_down(scancode, KMOD_RSHIFT, false), {}, "unsupported down cannot introduce a modifier");
        expect(state.key_up(scancode, KMOD_RSHIFT), {}, "unsupported up cannot change device state");
        check(state.held() == Report{4}, "unsupported scancodes preserve an existing held key");
    }
    expect(state.release_all(), {{}}, "cleanup after unsupported events");
}

void bitmap_has_no_six_key_limit() {
    KeyboardState state;
    Report expected;
    const std::array<SDL_Scancode, 8> keys{{SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C,
        SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H}};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        expected.push_back(static_cast<uint16_t>(4 + i));
        expect(state.key_down(keys[i], KMOD_NONE, false), {expected}, "bitmap retains more than six ordinary keys");
    }
    expect(state.key_down(SDL_SCANCODE_I, KMOD_RSHIFT, false),
           {{4, 5, 6, 7, 8, 9, 10, 11, 229}, {4, 5, 6, 7, 8, 9, 10, 11, 12, 229}},
           "modifier prefix preserves all eight existing keys");
    expect(state.release_all(), {{}}, "release every ordinary key and modifier in one report");
}

void pressed_owner_query() {
    KeyboardState state;
    check(!state.is_pressed(SDL_SCANCODE_A), "fresh ordinary key has no owner");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, true), {}, "repeat-only A is ignored");
    check(!state.is_pressed(SDL_SCANCODE_A), "repeat-only A does not become pressed");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_NONE, false), {{4}}, "assign device owner to A");
    check(state.is_pressed(SDL_SCANCODE_A), "device down has an owner");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_LALT, false, true), {}, "duplicate A cannot acquire local ownership");
    check(state.is_pressed(SDL_SCANCODE_A), "duplicate still reports the original press");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "release device owner A");
    check(!state.is_pressed(SDL_SCANCODE_A), "device up clears pressed owner");

    expect(state.key_down(SDL_SCANCODE_F, KMOD_LALT, false, true), {}, "assign local owner to F");
    check(state.is_pressed(SDL_SCANCODE_F) && state.held().empty(), "local owner exists without any device held key");
    expect(state.key_down(SDL_SCANCODE_F, KMOD_NONE, false), {}, "duplicate local F cannot forward");
    check(state.is_pressed(SDL_SCANCODE_F), "local owner survives changing modifiers");
    expect(state.key_up(SDL_SCANCODE_F, KMOD_NONE), {}, "release local F owner");
    check(!state.is_pressed(SDL_SCANCODE_F), "local F up permits a fresh action");

    expect(state.key_down(SDL_SCANCODE_LALT, KMOD_NONE, false), {}, "selected MOD is local");
    check(state.is_pressed(SDL_SCANCODE_LALT), "local modifier also has a pressed owner");
    expect(state.key_down(SDL_SCANCODE_A, KMOD_LSHIFT, false), {{225}, {4, 225}}, "infer device Shift while local Alt is held");
    check(state.is_pressed(SDL_SCANCODE_LSHIFT), "snapshot-inferred device modifier is pressed");
    expect(state.key_up(SDL_SCANCODE_A, KMOD_NONE), {{}}, "ordinary up clears snapshot Shift");
    check(!state.is_pressed(SDL_SCANCODE_LSHIFT), "released snapshot clears device modifier owner");
    check(state.is_pressed(SDL_SCANCODE_LALT), "snapshot removal does not clear local modifier ownership");
    expect(state.release_all(), {}, "cleanup also clears local-only owners");
    check(!state.is_pressed(SDL_SCANCODE_LALT), "release_all clears the local modifier query");

    for (const auto scancode : {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_AUDIOPLAY,
                               static_cast<SDL_Scancode>(SDL_NUM_SCANCODES),
                               static_cast<SDL_Scancode>(SDL_NUM_SCANCODES + 1)}) {
        expect(state.key_down(scancode, KMOD_NONE, false), {}, "unsupported query key is not accepted");
        check(!state.is_pressed(scancode), "invalid or unsupported scancode query is false");
    }
}

void geometry_release_keeps_local_ownership() {
    for (const auto mod : {std::pair{SDL_SCANCODE_LALT, uint16_t(KMOD_LALT)},
                           std::pair{SDL_SCANCODE_LGUI, uint16_t(KMOD_LGUI)}}) {
        KeyboardState state;
        expect(state.key_down(SDL_SCANCODE_A, KMOD_RSHIFT, false), {{229}, {4, 229}},
               "hold a device key and inferred modifier before resizing");
        expect(state.key_down(mod.first, mod.second, false), {}, "hold local MOD");
        expect(state.key_down(SDL_SCANCODE_G, mod.second, false, true), {}, "own local G");
        expect(state.key_down(SDL_SCANCODE_W, mod.second, false, true), {}, "own another local key");
        expect(state.release_device_keys(), {{}}, "resize releases device state once");
        check(state.held().empty() && !state.is_pressed(SDL_SCANCODE_A) &&
                  !state.is_pressed(SDL_SCANCODE_RSHIFT), "resize clears device owners and usages");
        check(state.is_pressed(mod.first) && state.is_pressed(SDL_SCANCODE_G) &&
                  state.is_pressed(SDL_SCANCODE_W), "resize preserves every held local owner");
        expect(state.release_device_keys(), {}, "repeated geometry release sends no empty report");
        expect(state.key_up(SDL_SCANCODE_A, KMOD_RSHIFT), {}, "old device up cannot revive Shift");
        expect(state.key_up(mod.first, KMOD_NONE), {}, "MOD can release before local keys");
        expect(state.key_down(SDL_SCANCODE_G, KMOD_NONE, false), {}, "duplicate local G stays local after MOD up");
        expect(state.key_down(SDL_SCANCODE_W, KMOD_NONE, true), {}, "local repeat stays local");
        expect(state.key_up(SDL_SCANCODE_W, KMOD_RCTRL), {}, "local W up cannot inject snapshot Control");
        expect(state.key_up(SDL_SCANCODE_G, KMOD_NONE), {}, "real G up clears local ownership");
        expect(state.key_down(SDL_SCANCODE_G, KMOD_NONE, false), {{10}}, "fresh G forwards after real up");
        expect(state.release_all(), {{}}, "normal cleanup still clears all state");
    }

    KeyboardState local_modifier(0);
    expect(local_modifier.key_down(SDL_SCANCODE_LSHIFT, KMOD_LSHIFT, false, true), {},
           "caller can own an otherwise device modifier");
    expect(local_modifier.release_device_keys(), {}, "local-only release produces no device report");
    expect(local_modifier.key_down(SDL_SCANCODE_A, KMOD_LSHIFT, false), {{4}},
           "preserved local modifier stays excluded from a new device key snapshot");
    expect(local_modifier.key_up(SDL_SCANCODE_LSHIFT, KMOD_NONE), {}, "local modifier up preserves A");
    expect(local_modifier.release_device_keys(), {{}}, "release A alone");
    expect(local_modifier.key_down(SDL_SCANCODE_F, KMOD_NONE, false, true), {}, "hold local F");
    expect(local_modifier.release_all(), {}, "focus cleanup also clears local-only ownership");
    check(!local_modifier.is_pressed(SDL_SCANCODE_F), "release_all remains distinct from geometry release");
}

} // namespace

int main() {
    basic_keys();
    modifier_prefix_preserves_keys();
    explicit_modifiers();
    shortcuts_keep_ownership();
    shortcut_modifier_filtering();
    repeats_release_and_fast_events();
    whitelist_and_lock_bits();
    bitmap_has_no_six_key_limit();
    pressed_owner_query();
    geometry_release_keeps_local_ownership();
    std::printf("keyboard_state: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
