// 测量接收静默后，wake() 到下一张新帧的耗时。
//
// FramePump 请求 20 秒 RTCP 空闲超时，正常产品每秒发送 RR 续期。
// 本探针默认关闭 RR 和自动重建，观察无反馈对照；--rr 保留生产保活。
// “请求了 20 秒”不能证明会话一定在起流后 20 秒结束，也不能仅凭静止视频判定断流。
// 这里按全部数据报的计数等待静默，并设置独立总时限；未观察到静默就终止实验。
#include "ProbeCli.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "ProbeWait.h"
#include "media/FramePump.h"
#include "remote/Device.h"

namespace {
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string udid;
    int trials = 5, quiet_target = 9000, wait_ms = 45000;
    bool verbose = false, rr = false, dry_run = false;
    CLI::App app("测量接收静默后 wake() 到新帧的延迟");
    app.add_option("UDID", udid, "设备 UDID");
    app.add_option("--trials", trials, "测量次数（默认 5）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--quiet", quiet_target, "数据报静默阈值，毫秒（默认 9000）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--wait-ms", wait_ms, "每次等待静默的观察预算，毫秒（默认 45000；不含连接与清理）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_flag("--rr", rr, "发送 RR 保活对照；可能在总时限内不出现静默");
    app.add_flag("-v,--verbose", verbose, "输出协议日志");
    app.add_flag("--dry-run", dry_run, "只显示实验参数，不连接设备");
    app.footer("需独占设备媒体服务：stopmediastream 使用 stopAll，会结束设备上的其它媒体会话。\n观察预算不包含连接、RPC 和停止媒体各自的协议时限。");
    try { app.parse(argc, argv); }
    catch (const CLI::CallForHelp &e) { return app.exit(e); }
    catch (const CLI::ParseError &e) { std::fprintf(stderr, "参数错误：%s\n", e.what()); return 2; }
    if (wait_ms <= quiet_target) {
        std::fprintf(stderr, "--wait-ms 必须大于 --quiet，才能在总时限前观察到静默\n");
        return 2;
    }
    std::printf("实验参数：timeout=20 秒，RR=%s，trials=%d，quiet_ms=%d，wait_ms=%d\n",
                rr ? "on" : "off", trials, quiet_target, wait_ms);
    if (dry_run) { return 0; }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    // 自动重建与 RR 是不同开关。两项重建都关闭后，还需单独禁止 RR，
    // 才能观察无反馈到期；保活对照则只改变这一项。
    scrctl::media::FramePump::Options options;
    options.silence_restart_ms = 0;
    options.stall_restart_ms = 0;
    options.debug_suppress_rr = !rr;
    auto pump = scrctl::media::FramePump::start(*device, options, err, verbose);
    if (!pump) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    scrctl::Frame first;
    if (!pump->latest(first, 3000)) {
        std::fprintf(stderr, "没有第一帧\n");
        return 1;
    }

    std::vector<int> samples;
    for (int trial = 0; trial < trials; ++trial) {
        scrctl::probe::QuietWait wait(now_ms(), pump->stats().packets, quiet_target, wait_ms);
        for (;;) {
            const uint64_t now = now_ms();
            const auto result = wait.observe(now, pump->stats().packets);
            if (result == scrctl::probe::QuietResult::TimedOut) {
                std::fprintf(stderr, "第 %d 次：%dms 内未观察到数据报静默；未调用 wake()，本次没有恢复延迟样本\n",
                             trial + 1, wait_ms);
                return 1;
            }
            if (result == scrctl::probe::QuietResult::Quiet) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(250, wait.remaining(now))));
        }
        const uint64_t quiet = wait.quiet_for(now_ms());
        const auto before_stats = pump->stats();
        const uint64_t before = pump->serial();
        const auto t0 = now_ms();
        pump->wake();
        scrctl::Frame f;
        uint64_t got_serial = 0;
        bool saw_reviving = false;
        scrctl::probe::Deadline recovery(t0, 8000);
        while (!recovery.expired(now_ms())) {
            saw_reviving = saw_reviving || pump->reviving();
            got_serial = pump->newer(f, before, 20);
            if (got_serial != 0) { break; }
        }
        const int ms = static_cast<int>(now_ms() - t0);
        if (got_serial == 0 || recovery.expired(now_ms())) {
            std::fprintf(stderr, "第 %d 次：静默 %llums 后 wake()，8 秒内没有新帧\n", trial + 1,
                         static_cast<unsigned long long>(quiet));
            return 1;
        }
        samples.push_back(ms);
        const auto stats = pump->stats();
        std::printf("第 %d 次：静默 %llums -> 新帧 %dms（%s，帧号 %llu -> %llu，重建增量 %llu，RR 成功 %llu/失败 %llu）\n",
                    trial + 1, static_cast<unsigned long long>(quiet), ms,
                    saw_reviving ? "观察到重建" : "未观察到重建",
                    static_cast<unsigned long long>(before), static_cast<unsigned long long>(got_serial),
                    static_cast<unsigned long long>(stats.restarts - before_stats.restarts),
                    static_cast<unsigned long long>(stats.rtcp_sent), static_cast<unsigned long long>(stats.rtcp_failed));
    }
    std::sort(samples.begin(), samples.end());
    std::printf("\n%d 次：中位 %dms，最大 %dms；这些是本次配置下的观测值\n",
                static_cast<int>(samples.size()), samples[samples.size() / 2], samples.back());
    return 0;
}
