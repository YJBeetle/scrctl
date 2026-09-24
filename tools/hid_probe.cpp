// 探针：把 HID 注入这条路走通，并在真机上看到结果。
//
// 判据不是"设备没报错"——send 是只发不收的，链路层永远"成功"。真正的判据是
// 屏幕上出现我们画的东西：先在无边记里画一笔，再截图比对（这套按带比对的读法
// 见 tools/hid_gate_probe + gate_diff.py）。默认顺手起一条媒体流只是为了让画面
// 有人看着；"没流就注入不进去"这条早先的结论已经被 hid_gate_probe 推翻。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

/// 视频流的载荷没人看，但**必须**把包读干净：不读的话设备侧中继缓冲溢出。
class Drainer {
public:
    explicit Drainer(scrctl::media::StreamSession &session) : session_(session) {
        worker_ = std::thread([this] {
            std::string err;
            std::vector<uint8_t> packet;
            while (!stopping_) {
                if (session_.next_packet(packet, 50, err)) {
                    ++packets_;
                }
            }
        });
    }
    ~Drainer() {
        stopping_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    [[nodiscard]] unsigned long long packets() const { return packets_; }

private:
    scrctl::media::StreamSession &session_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    unsigned long long packets_ = 0;
};

void usage(const char *argv0) {
    std::printf(
        "用法: %s [-v] [--list] [--tap X Y] [--stroke] [--line X0 Y0 X1 Y1] [--no-stream] [UDID]\n"
        "\n"
        "  --list    列出设备注册的 HID 面\n"
        "  --tap     在归一化坐标 (0..1) 点一下，默认按住 90ms\n"
        "  --stroke  在屏幕中央画一条短斜线（验证画面真的收到了触摸）\n"
        "  --line    画一条插值直线，用于注入前后的截图对比\n"
        "  --probe-reply  一发一收地发一对报告，把设备的回信原样打出来\n"
        "  --no-stream  不起流。用来复验注入到底要不要一条在跑的流（结论：不要，见 hid_gate_probe）\n"
        "  --swipe-loop N  连续横向拖动 N 秒。在无边记里它就是平移画布，是一个\n"
        "                    可控的持续高运动画面源（量帧率时不用它就没法排除\n"
        "                    \'画面本来没在动\'）\n",
        argv0);
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool verbose = false;
    bool list = false;
    bool raw_surfaces = false;
    bool stroke = false;
    bool line = false;
    bool probe_reply = false;
    std::string keys;
    bool paste = false;
    double lx0 = 0.2, ly0 = 0.66, lx1 = 0.32, ly1 = 0.70;
    bool with_stream = true;
    int swipe_seconds = 0;
    bool want_tap = false;
    double tx = 0.5, ty = 0.5;
    std::string serial;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--list") {
            list = true;
        } else if (a == "--raw") {
            raw_surfaces = true;
        } else if (a == "--tap") {
            want_tap = true;
            if (i + 2 < argc) {
                tx = std::atof(argv[i + 1]);
                ty = std::atof(argv[i + 2]);
                i += 2;
            }
        } else if (a == "--stroke") {
            stroke = true;
        } else if (a == "--line" && i + 4 < argc) {
            line = true;
            lx0 = std::atof(argv[i + 1]);
            ly0 = std::atof(argv[i + 2]);
            lx1 = std::atof(argv[i + 3]);
            ly1 = std::atof(argv[i + 4]);
            i += 4;
        } else if (a == "--paste") {
            paste = true;
        } else if (a == "--keys" && i + 1 < argc) {
            keys = argv[++i];
        } else if (a == "--probe-reply") {
            probe_reply = true;
        } else if (a == "--no-stream") {
            with_stream = false;
        } else if (a == "--swipe-loop" && i + 1 < argc) {
            swipe_seconds = std::stoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            serial = a;
        }
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(serial, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    std::unique_ptr<scrctl::media::StreamSession> session;
    std::unique_ptr<Drainer> drainer;
    if (with_stream) {
        scrctl::media::StreamSession::Request req;
        session = scrctl::media::StreamSession::start(*device, req, err, verbose);
        if (session == nullptr) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            return 1;
        }
        drainer = std::make_unique<Drainer>(*session);
        // 面的认证状态是流起来之后才翻的，立刻发报告会被丢掉。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf("流在跑，已收 %llu 个包\n", drainer->packets());
    } else {
        std::printf("按 --no-stream 起了个**没有**流的会话\n");
    }

    auto hid = scrctl::hid::Service::open(*device, err, verbose);
    if (hid == nullptr) {
        std::fprintf(stderr, "打开 HID 服务失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("universalhidservice 已连接\n");

    if (list || raw_surfaces) {
        if (raw_surfaces) {
            // 把 connectedServices 的原文整个交出来。注入门开没开，答案很可能
            // 就写在这些条目的某个属性里（authenticated / eventSource 之类），
            // 而只挑自己预先想到的键来看的话，永远看不到它。
            scrctl::xpc::Value reply;
            if (hid->raw_connected_services(reply, err)) {
                std::printf("connectedServices 原文:\n%s\n",
                            scrctl::xpc::describe(reply).c_str());
            } else {
                std::printf("取原文失败: %s\n", err.c_str());
            }
        }
        std::vector<scrctl::hid::Service::Surface> surfaces;
        if (!hid->surfaces(surfaces, err)) {
            std::fprintf(stderr, "列面失败: %s\n", err.c_str());
        }
        for (const auto &s : surfaces) {
            std::printf("  面 %llu (0x%llx)  %s\n", static_cast<unsigned long long>(s.service_id),
                        static_cast<unsigned long long>(s.service_id), s.name.c_str());
        }
    }

    int rc = 0;
    if (want_tap) {
        std::printf("点击 (%.3f, %.3f)\n", tx, ty);
        if (!hid->tap(tx, ty, 90, err)) {
            std::fprintf(stderr, "点击失败: %s\n", err.c_str());
            rc = 1;
        }
    }
    if (stroke) {
        // 只在画面正中一小段，不去够系统边缘——边缘那几条手势会叫出控制中心。
        std::printf("画一条中央短斜线\n");
        const std::vector<std::pair<double, double>> pts = {{0.44, 0.44}, {0.47, 0.46},
                                                            {0.50, 0.48}, {0.53, 0.50},
                                                            {0.56, 0.52}};
        if (!hid->stroke(pts, 16, err)) {
            std::fprintf(stderr, "拖动失败: %s\n", err.c_str());
            rc = 1;
        }
    }
    if (line) {
        // 一条插值出来的直线，用来做"注入前后截图对比"这种可判定的实验。
        std::printf("画线 (%.3f,%.3f) -> (%.3f,%.3f)\n", lx0, ly0, lx1, ly1);
        std::vector<std::pair<double, double>> pts;
        for (int i = 0; i <= 24; ++i) {
            const double t = i / 24.0;
            pts.emplace_back(lx0 + (lx1 - lx0) * t, ly0 + (ly1 - ly0) * t);
        }
        if (!hid->stroke(pts, 12, err)) {
            std::fprintf(stderr, "画线失败: %s\n", err.c_str());
            rc = 1;
        }
    }
    if (swipe_seconds > 0) {
        // 横向拖动画布是一个**零风险的持续高运动画面源**（无边记里就是平移视图，
        // 用户已明确允许在这个画布上操作）。为什么需要它：量到"scrctl 的会话只有
        // 12 帧/秒"时，第一个要排除的解释就是"画面本来就没在动"——只有喂一个确定
        // 在动的内容，12 帧这个数才有意义。
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(swipe_seconds);
        bool rightward = true;
        long flips = 0;
        while (std::chrono::steady_clock::now() < until) {
            const double x0 = rightward ? 0.82 : 0.18;
            const double x1 = rightward ? 0.18 : 0.82;
            std::vector<std::pair<double, double>> pts;
            for (int i = 0; i <= 16; ++i) {
                const double t = i / 16.0;
                pts.emplace_back(x0 + (x1 - x0) * t, 0.55);
            }
            std::string serr;
            if (!hid->stroke(pts, 14, serr)) {
                std::fprintf(stderr, "拖动失败: %s\n", serr.c_str());
                rc = 1;
                break;
            }
            rightward = !rightward;
            ++flips;
            std::this_thread::sleep_for(std::chrono::milliseconds(240));
        }
        std::printf("横向拖了 %ld 次\n", flips);
    }
    if (!keys.empty()) {
        // 键盘面：先试设备自带的那个（list 里 512 是 "CoreDevice keyboard"）。
        const uint64_t surface = std::strtoull(keys.c_str(), nullptr, 10);
        std::printf("在面 %llu 上敲 a b c\n", static_cast<unsigned long long>(surface));
        for (uint16_t usage : {scrctl::hid::key::kA, uint16_t { scrctl::hid::key::kA + 1 }, uint16_t { scrctl::hid::key::kA + 2 }}) {
            if (!hid->type(surface, {usage}, 60, err)) {
                std::fprintf(stderr, "敲键失败: %s\n", err.c_str());
                rc = 1;
                break;
            }
        }
    }
    if (probe_reply) {
        // 一发一收地试：设备的态度只有这样才能拿到。send 是只发不收的，格式错了
        // 也"成功"，屏幕却没反应，现场什么都看不出来。
        for (const auto state : {scrctl::hid::kStateContact, scrctl::hid::kStateRelease}) {
            scrctl::xpc::Value reply;
            const auto report = scrctl::hid::touchscreen_report(
                state, scrctl::hid::normalize(0.5), scrctl::hid::normalize(0.5));
            const bool ok = hid->send_report(scrctl::hid::kSurfaceMainTouchscreen, report, err,
                                             &reply);
            std::printf("报告 state=0x%02x: %s\n", state, ok ? "有回信" : err.c_str());
            if (ok) {
                std::printf("  回信: %s\n", scrctl::xpc::describe(reply).substr(0, 500).c_str());
            }
        }
    }
    if (paste) {
        // Command+V。iOS 接了外接键盘时这是通用粘贴手势，所以"剪贴板 + 一次按键"
        // 就是非 ASCII 文本的输入路径。
        std::printf("发 Command+V\n");
        if (!hid->press_chord(scrctl::hid::kSurfaceKeyboard,
                              { scrctl::hid::key::kGuiLeft,
                                uint16_t { scrctl::hid::key::kA + 21 } },  // v
                              60, err)) {
            std::fprintf(stderr, "粘贴失败: %s\n", err.c_str());
            rc = 1;
        }
    }
    if (!list && !want_tap && !stroke && !line && !probe_reply && !raw_surfaces &&
        keys.empty() && !paste && swipe_seconds == 0) {
        usage(argv[0]);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (with_stream) {
        std::printf("结束时流已收 %llu 个包\n", drainer->packets());
    }
    return rc;
}
