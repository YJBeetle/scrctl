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

// 探针动作按命令行出现的顺序执行。为什么必须能排队：唤醒-截图-手势-截图这套判据
// 要是拆成几个进程，每个进程各自建会话要一两秒，而这台设备几秒就睡——基线还没拍
// 屏幕就黑了，差分全是假的（2026-09-28 在 iPadOS 18 那台上就这样作废了两轮）。
struct QueuedAction {
    enum Kind { kButton, kTap, kLine, kShot } kind;
    double v[4] = {0, 0, 0, 0};
    std::string arg;
};

void usage(const char *argv0) {
    std::printf(
        "用法: %s [-v] [--list] [--tap X Y] [--stroke] [--line X0 Y0 X1 Y1] [--button NAME]\n"
        "           [--shot FILE] [--no-stream] [UDID]\n"
        "\n"
        "  --list    列出设备注册的 HID 面\n"
        "  --tap     在归一化坐标 (0..1) 点一下，默认按住 90ms\n"
        "  --stroke  在屏幕中央画一条短斜线（验证画面真的收到了触摸）\n"
        "  --line    画一条插值直线，用于注入前后的截图对比\n"
        "  --button  按一次硬件按键（home/lock/volup/voldn/mute）。息屏的设备只能靠\n"
        "            按键唤醒，而唤醒本身又是「注入落没落地」最干净的判据\n"
        "  --shot    在同一进程里抓一张 screencaptureservice 的 PNG 存到 FILE\n"
        "\n"
        "  --button/--tap/--line/--shot **按命令行出现的顺序**执行。这台 iPad 几秒就睡，\n"
        "  而每个探针进程各自建会话要一两秒——「唤醒-截图-手势-截图」必须压在一个进程里\n"
        "  才跑赢自动锁屏，跨进程做差分拿到的基线是假的。\n"
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
    bool probe_reply = false;
    std::string keys;
    bool paste = false;
    bool with_stream = true;
    int swipe_seconds = 0;
    std::vector<QueuedAction> queue;
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
            QueuedAction q;
            q.kind = QueuedAction::kTap;
            if (i + 2 < argc) {
                q.v[0] = std::atof(argv[i + 1]);
                q.v[1] = std::atof(argv[i + 2]);
                i += 2;
            } else {
                q.v[0] = q.v[1] = 0.5;
            }
            queue.push_back(q);
        } else if (a == "--stroke") {
            stroke = true;
        } else if (a == "--button" && i + 1 < argc) {
            QueuedAction q;
            q.kind = QueuedAction::kButton;
            q.arg = argv[++i];
            queue.push_back(q);
        } else if (a == "--shot" && i + 1 < argc) {
            QueuedAction q;
            q.kind = QueuedAction::kShot;
            q.arg = argv[++i];
            queue.push_back(q);
        } else if (a == "--line" && i + 4 < argc) {
            QueuedAction q;
            q.kind = QueuedAction::kLine;
            q.v[0] = std::atof(argv[i + 1]);
            q.v[1] = std::atof(argv[i + 2]);
            q.v[2] = std::atof(argv[i + 3]);
            q.v[3] = std::atof(argv[i + 4]);
            i += 4;
            queue.push_back(q);
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
    for (const auto &qa : queue) {
        switch (qa.kind) {
        case QueuedAction::kButton: {
            static const std::pair<const char *, uint16_t> kCodes[] = {
                {"home", scrctl::hid::button::kHome},   {"lock", scrctl::hid::button::kLock},
                {"volup", scrctl::hid::button::kVolumeUp},
                {"voldn", scrctl::hid::button::kVolumeDown}, {"mute", scrctl::hid::button::kMute},
            };
            uint16_t code = 0;
            for (const auto &e : kCodes) {
                if (qa.arg == e.first) {
                    code = e.second;
                    break;
                }
            }
            if (code == 0) {
                std::fprintf(stderr, "不认识按键 %s（可用：home/lock/volup/voldn/mute）\n",
                             qa.arg.c_str());
                return 2;
            }
            auto buttons = scrctl::hid::Buttons::open(*device, err, verbose);
            if (buttons == nullptr) {
                std::fprintf(stderr, "打开按键面失败: %s\n", err.c_str());
                return 1;
            }
            std::printf("按硬件键 %s\n", qa.arg.c_str());
            if (!buttons->press(scrctl::hid::button::kUsagePageConsumer, code, 90, err)) {
                std::fprintf(stderr, "按键失败: %s\n", err.c_str());
                rc = 1;
            }
            break;
        }
        case QueuedAction::kTap:
            std::printf("点击 (%.3f, %.3f)\n", qa.v[0], qa.v[1]);
            if (!hid->tap(qa.v[0], qa.v[1], 90, err)) {
                std::fprintf(stderr, "点击失败: %s\n", err.c_str());
                rc = 1;
            }
            break;
        case QueuedAction::kLine: {
            std::printf("画线 (%.3f,%.3f) -> (%.3f,%.3f)\n", qa.v[0], qa.v[1], qa.v[2], qa.v[3]);
            std::vector<std::pair<double, double>> pts;
            for (int i = 0; i <= 24; ++i) {
                const double t = i / 24.0;
                pts.emplace_back(qa.v[0] + (qa.v[2] - qa.v[0]) * t,
                                 qa.v[1] + (qa.v[3] - qa.v[1]) * t);
            }
            if (!hid->stroke(pts, 12, err)) {
                std::fprintf(stderr, "画线失败: %s\n", err.c_str());
                rc = 1;
            }
            break;
        }
        case QueuedAction::kShot: {
            // 与 screenshot_probe 同一条 callscreenshot，只是活在**这个**进程里：
            // 判据要求截图与手势之间只隔毫秒，跨进程的一两秒会被自动锁屏抢跑。
            auto input = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
            scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));
            scrctl::xpc::Value out;
            if (!device->feature("com.apple.coredevice.screencaptureservice",
                                 "com.apple.coredevice.feature.capturescreenshot",
                                 "com.apple.coredevice.action.capturescreenshot", input, out, err,
                                 verbose, 15000)) {
                std::fprintf(stderr, "截图失败: %s\n", err.c_str());
                rc = 1;
                break;
            }
            const auto *image = out.find("image");
            if (image == nullptr || image->data.empty()) {
                std::fprintf(stderr, "截图回信里没有 image 字节\n");
                rc = 1;
                break;
            }
            std::FILE *fp = std::fopen(qa.arg.c_str(), "wb");
            if (fp == nullptr) {
                std::fprintf(stderr, "写不了 %s\n", qa.arg.c_str());
                rc = 1;
                break;
            }
            std::fwrite(image->data.data(), 1, image->data.size(), fp);
            std::fclose(fp);
            std::printf("截图 %zu 字节 -> %s\n", image->data.size(), qa.arg.c_str());
            break;
        }
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
    if (!list && queue.empty() && !stroke && !probe_reply && !raw_surfaces &&
        keys.empty() && !paste && swipe_seconds == 0) {
        usage(argv[0]);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (with_stream) {
        std::printf("结束时流已收 %llu 个包\n", drainer->packets());
    }
    return rc;
}
