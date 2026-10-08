// 探针：视频流取帧 vs screencaptureservice 抓图，同一个会话里对着比。
//
// 为什么值得单独测：MaaFramework 的整个循环是"截图 -> 识别 -> 动作"，截图有多快
// 直接决定任务能跑多快。screencaptureservice 实测一张要 344~613ms，而视频流是
// 60fps——如果取"最新一帧"确实便宜，控制器就该走流而不是走截图服务。
// 这个判断得用数字定，不能靠"流是 60fps 所以一定快"这种推理。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "decode/Decoder.h"
#include "media/FramePump.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

using clock = std::chrono::steady_clock;

double ms_since(clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

/// 中位数比均值抗噪：偶发一次 200ms 的调度延迟不该把结论带偏。
double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int frames = 30;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
        else if (a == "-n" && i + 1 < argc) {
            frames = std::stoi(argv[++i]);
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    // ---- 1. 视频流取帧 --------------------------------------------------------
    scrctl::media::FramePump::Options po;
    po.stall_restart_ms = 0;  // 测延迟，别在测量中间重起会话
    const auto t_start = clock::now();
    auto pump = scrctl::media::FramePump::start(*dev, po, err, verbose);
    if (pump == nullptr) {
        std::fprintf(stderr, "起泵失败: %s\n", err.c_str());
        return 1;
    }

    scrctl::Frame first;
    if (!pump->latest(first, 5000)) {
        std::fprintf(stderr, "5 秒内没解出第一帧\n");
        return 1;
    }
    std::printf("\n第一帧：%5.0f ms（含建会话、起流、等参数集、解第一帧）  %ux%u\n",
                ms_since(t_start), first.width, first.height);

    std::vector<double> fresh;
    uint64_t serial = pump->serial();
    for (int i = 0; i < frames; ++i) {
        const auto t0 = clock::now();
        scrctl::Frame f;
        const uint64_t got = pump->newer(f, serial, 2000);
        if (got == 0) {
            std::printf("第 %d 帧超时\n", i);
            break;
        }
        fresh.push_back(ms_since(t0));
        serial = got;
    }
    const auto st = pump->stats();
    std::printf("等下一帧的延迟：中位 %.1f ms  最小 %.1f  最大 %.1f  （n=%zu）\n", median(fresh),
                fresh.empty() ? 0.0 : *std::min_element(fresh.begin(), fresh.end()),
                fresh.empty() ? 0.0 : *std::max_element(fresh.begin(), fresh.end()), fresh.size());
    std::printf("取完 %d 帧期间：包 %llu 解码 %llu 未出帧 %llu\n", frames,
                static_cast<unsigned long long>(st.packets),
                static_cast<unsigned long long>(st.decoded),
                static_cast<unsigned long long>(st.no_output));

    // 取帧不等待的情况：连续问 N 次"最新一帧"，看吞吐上限。
    const auto t_spin = clock::now();
    int spins = 0;
    for (int i = 0; i < 200; ++i) {
        scrctl::Frame f;
        if (pump->latest(f, 0)) {
            ++spins;
        }
    }
    std::printf("不等待地取帧：200 次里 %d 次拿到，%.2f 秒/万次（这就是「直接拷贝一帧」的开销）\n",
                spins, ms_since(t_spin) * 10.0);

    // ---- 2. screencaptureservice ---------------------------------------------
    std::vector<double> shots;
    for (int i = 0; i < 8; ++i) {
        auto input = scrctl::xpc::make_dict();
        scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
        scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));
        scrctl::xpc::Value out;
        const auto t0 = clock::now();
        if (!dev->feature("com.apple.coredevice.screencaptureservice",
                          "com.apple.coredevice.feature.capturescreenshot",
                          "com.apple.coredevice.action.capturescreenshot", input, out, err,
                          verbose, 30000)) {
            std::fprintf(stderr, "截图失败: %s\n", err.c_str());
            break;
        }
        shots.push_back(ms_since(t0));
    }
    std::printf("\nscreencaptureservice 抓图：中位 %.0f ms  最小 %.0f  最大 %.0f  （n=%zu）\n",
                median(shots), shots.empty() ? 0.0 : *std::min_element(shots.begin(), shots.end()),
                shots.empty() ? 0.0 : *std::max_element(shots.begin(), shots.end()), shots.size());

    if (!shots.empty() && !fresh.empty()) {
        std::printf("\n结论：走视频流取帧比走截图服务快 %.1f 倍。\n",
                    median(shots) / std::max(0.1, median(fresh)));
    }

    std::printf("\n重起之后统计: 重启 %llu 次，序号断流 %llu\n",
                static_cast<unsigned long long>(pump->stats().restarts),
                static_cast<unsigned long long>(pump->stats().gaps));
    return 0;
}
