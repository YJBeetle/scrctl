#include "ProbeWait.h"
#include "ProbeCli.h"

#include <cstdio>
#include <cstdint>
#include <limits>

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char *what) {
        ++checks;
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    };
    using scrctl::probe::Deadline;
    using scrctl::probe::QuietResult;
    using scrctl::probe::QuietWait;

    Deadline deadline(700, 50);
    check(!deadline.expired(749) && deadline.remaining(749) == 1, "last millisecond remains");
    check(deadline.expired(750) && deadline.remaining(750) == 0, "deadline includes equality");
    check(deadline.expired(751) && deadline.remaining(751) == 0, "remaining time saturates at zero");

    // 不断来包不能无限推迟总截止；这也是 lifetime 每轮收包的退出判据。
    QuietWait busy(1000, 0, 10, 100);
    for (uint64_t i = 1; i < 100; ++i) {
        check(busy.observe(1000 + i, i) == QuietResult::Pending, "incoming packets reset only quiet time");
    }
    check(busy.observe(1100, 100) == QuietResult::TimedOut, "continuous packets reach total timeout");
    check(busy.remaining(1100) == 0, "busy stream has no remaining time");

    QuietWait idle(2000, 4, 10, 100);
    check(idle.observe(2009, 4) == QuietResult::Pending, "quiet threshold not reached early");
    check(idle.observe(2010, 4) == QuietResult::Quiet, "quiet threshold includes equality");
    check(idle.observe(2011, 5) == QuietResult::Pending && idle.quiet_for(2011) == 0,
          "new packet cancels quiet observation");
    QuietWait tied(0, 0, 10, 10);
    check(tied.observe(10, 0) == QuietResult::TimedOut, "total deadline takes precedence over quiet");

    const uint64_t long_ms = uint64_t(std::numeric_limits<int>::max()) * 1000;
    Deadline long_wait(1, long_ms);
    check(!long_wait.expired(long_ms) && long_wait.expired(long_ms + 1), "seconds convert before multiplication");
    // 单调计数器回绕也不需要构造可能溢出的绝对截止值。
    Deadline wrapped(std::numeric_limits<uint64_t>::max() - 4, 10);
    check(!wrapped.expired(4) && wrapped.expired(5), "deadline comparison uses elapsed time");
    auto decimal = scrctl::probe::decimal_integer(0, std::numeric_limits<int>::max());
    for (const auto *input : {"010", "+010", " 010 "}) {
        std::string value = input;
        check(decimal(value).empty() && value == "10", "decimal normalization preserves ten");
    }
    for (const auto *input : {"0x10", "10x", "", "1.5", "-1", "+-0", "++1", "2147483648", "999999999999999999999"}) {
        std::string value = input;
        check(!decimal(value).empty(), "invalid or overflowing decimal rejected");
    }
    auto signed_decimal = scrctl::probe::decimal_integer(-1, std::numeric_limits<int>::max());
    std::string double_sign = "+-1";
    check(!signed_decimal(double_sign).empty(), "double sign rejected even when negative values allowed");
    CLI::App app;
    int parsed = 0;
    app.add_option("--seconds", parsed)->transform(decimal);
    const char *args[] = {"probe", "--seconds", "010"};
    app.parse(3, args);
    check(parsed == 10, "normalized decimal reaches real CLI11 integer conversion");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
