// 探针：从"催一次流"到"拿到重起后的第一帧"到底要多久。
//
// 为什么单独量：自动化框架的循环是"截图 -> 识别 -> 动作"，而它的截图调用在等帧时
// 用的是一个**固定预算**（MaaFW 那边是 800ms）。设备会在最后一个视频包之后约 7 秒
// 结束整条会话（docs §13），所以"隔了一会儿再截图"这个最常见不过的时序，必然要走
// 一次重起。预算给小了，截图就会退回"最新一帧"——也就是设备拆流前那一刻的旧画面，
// 而调用方以为看到的是刚刚动作之后的结果。这个数必须量出来，不能猜。
//
// 用法：wake_latency_probe [--trials 5] [--verbose] [UDID]
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "media/FramePump.h"
#include "remote/Device.h"

namespace {

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string_view udid;
    int trials = 5;
    // 静默多久之后催一次。泵里"该不该重起"的判定用的是"最后一个数据报静默满 2 秒"
    // （设备的 RTCP SR 每秒一个，连它都停了才算死），所以这个参数扫过 2 秒上下，
    // 就能看出阈值是不是卡在正确的位置上：3000ms 那一档必须和 9000ms 那一档一样
    // 一次就重起成功。
    int quiet_target = 9000;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--trials" && i + 1 < argc) {
            trials = std::atoi(argv[++i]);
        } else if (a == "--quiet" && i + 1 < argc) {
            quiet_target = std::atoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::printf("用法: %s [--trials N] [--quiet 毫秒] [-v] [UDID]\n", argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", std::string(a).c_str());
            return 2;
        } else {
            udid = a;
        }
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }

    // silence/stall 重起全关掉：这个探针要的就是"会话确实死透了，然后由 wake()
    // 把它救回来"这一段，不能让泵自己先救。
    scrctl::media::FramePump::Options options;
    options.silence_restart_ms = 0;
    options.stall_restart_ms = 0;
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
    for (int trial = 1; trial <= trials; ++trial) {
        // 等到"一个包都不来"满 quiet_target。设备的 RTCP SR 每秒一个，所以
        //   quiet_target=1500 落在"可疑区间"（该去问设备那一条）
        //   quiet_target=4000 落在"两个心跳都没了"（该直接重起那一条）
        // 两档都必须催回一帧，只是花的钱不一样。
        uint64_t last_pkts = pump->stats().packets;
        uint64_t last_change = now_ms();
        while (now_ms() - last_change < static_cast<uint64_t>(quiet_target)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uint64_t got = pump->stats().packets;
            if (got != last_pkts) {
                last_pkts = got;
                last_change = now_ms();
            }
        }
        const uint64_t quiet = now_ms() - last_change;

        const uint64_t before = pump->serial();
        const auto t0 = now_ms();
        pump->wake();
        scrctl::Frame f;
        const uint64_t got_serial = pump->newer(f, before, 8000);
        const int ms = static_cast<int>(now_ms() - t0);
        if (got_serial == 0) {
            std::printf("第 %d 次：静默 %llums 后 wake()，8 秒内没有帧 —— 失败\n", trial,
                        static_cast<unsigned long long>(quiet));
            continue;
        }
        samples.push_back(ms);
        std::printf("第 %d 次：静默 %llums -> wake() 到第一帧 %dms（帧号 %llu -> %llu）\n", trial,
                    static_cast<unsigned long long>(quiet), ms,
                    static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(got_serial));
    }

    if (samples.empty()) {
        std::fprintf(stderr, "一次都没量到\n");
        return 1;
    }
    std::sort(samples.begin(), samples.end());
    std::printf("\n%d 次：中位 %dms，最大 %dms\n", static_cast<int>(samples.size()),
                samples[samples.size() / 2], samples.back());
    std::printf("截图等帧预算要大于这个最大值的两倍（重起期间设备屏幕还可能继续变）。\n");
    return 0;
}
