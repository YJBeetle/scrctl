// 探针：触摸注入的"认证门"到底挂在什么上——挂在"曾经起过一条流"，还是挂在
// "此刻有一条流活着"。
//
// 为什么要问这个：实测设备会在起流后约 20 秒把整条媒体会话结束掉（docs §13；静止画面
// 上最后一个视频包之后约 7 秒就是这道线，所以"屏幕停了一会儿再点一下"必然常常落在
// 会话已经死了的时候）。如果那条门是跟着会话生死走的，那么自动化框架里这个最常见不过
// 的时序，点下去会被 backboardd 静默丢掉——而 send 是只发不收的，这边毫无感觉。这比
// "截图截到旧帧"严重得多。
//
// 判据不能是"设备没报错"，也不能是视频流（会话本来就是死的）。所以用
// screencaptureservice 抓图：它走的是另一条服务通道，与会话生死无关，而画面上有
// 没有我们那一笔是一眼能判定的事实。每条线画在不同的纵向带上，事后按带比对
// （gate_diff.py），就能把"哪一次注入落地了"分别归到各次实验上。
//
// 前置条件：设备停在无边记的画笔界面（黑底白笔，差异最明显）。
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
    std::atomic<bool> stopping_ { false };
    unsigned long long packets_ = 0;
};

using clock = std::chrono::steady_clock;

double elapsed_ms(const clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

void log(const clock::time_point &t0, const std::string &what) {
    std::printf("[%8.1f ms] %s\n", elapsed_ms(t0), what.c_str());
    std::fflush(stdout);
}

/// 抓一张屏幕存成 PNG。走的是 screencaptureservice，与媒体会话无关。
bool capture(scrctl::remote::Device &device, const std::string &path, std::string &err) {
    auto input = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
    scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));

    scrctl::xpc::Value out;
    if (!device.feature("com.apple.coredevice.screencaptureservice",
                        "com.apple.coredevice.feature.capturescreenshot",
                        "com.apple.coredevice.action.capturescreenshot", input, out, err, false,
                        30000)) {
        return false;
    }
    const auto *image = out.find("image");
    if (image == nullptr || image->data.empty()) {
        err = "回信里没有 image 字节";
        return false;
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        err = "打不开 " + path;
        return false;
    }
    std::fwrite(image->data.data(), 1, image->data.size(), f);
    std::fclose(f);
    return true;
}

/// 一条插值直线。坐标是整块屏幕的 0..1。
bool draw_line(scrctl::hid::Service &hid, double x0, double y0, double x1, double y1,
               std::string &err) {
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i <= 24; ++i) {
        const double t = i / 24.0;
        pts.emplace_back(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
    }
    return hid.stroke(pts, 12, err);
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 用法：hid_gate_probe [-v] [--dir 输出目录] [UDID]
    std::string_view udid;
    std::string dir = "/tmp/gate";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "-h" || a == "--help") {
            std::printf("用法: %s [-v] [--dir 目录] [UDID]\n", argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", std::string(a).c_str());
            return 2;
        } else {
            udid = a;
        }
    }
    ::system(("mkdir -p " + dir).c_str());

    const auto t0 = clock::now();
    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    log(t0, "会话就绪 " + device->property("ProductType"));

    // 四条实验各占一条纵向带，互不重叠，事后按带归因。
    constexpr double kControlY = 0.26;
    constexpr double kIdleDeadY = 0.42;
    constexpr double kGoneY = 0.58;
    constexpr double kRevivedY = 0.74;
    constexpr double kSlope = 0.05;
    const double xa = 0.55, xb = 0.88;

    scrctl::hid::Service *hid_ptr = nullptr;
    auto shot = [&](const char *name) {
        const auto ts = clock::now();
        std::string cerr;
        const std::string path = dir + "/" + name + ".png";
        if (!capture(*device, path, cerr)) {
            std::fprintf(stderr, "抓图 %s 失败: %s\n", name, cerr.c_str());
            std::exit(1);
        }
        log(t0, std::string("抓图 ") + name + " 用时 " +
                        std::to_string(static_cast<int>(elapsed_ms(ts))) + "ms");
    };

    auto draw = [&](const char *name, double y) {
        std::string derr;
        if (!draw_line(*hid_ptr, xa, y, xb, y + kSlope, derr)) {
            std::fprintf(stderr, "%s 画线失败: %s\n", name, derr.c_str());
        }
        log(t0, std::string("已注入 ") + name + " 那条线");
    };

    // ---- 阶段 1：起流，等认证门 ----
    scrctl::media::StreamSession::Request req;
    auto session = scrctl::media::StreamSession::start(*device, req, err, verbose);
    if (session == nullptr) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    // 读线程持有 session 的引用，所以拆的时候必须先停它。
    auto drainer = std::make_unique<Drainer>(*session);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    log(t0, "流在跑");

    auto hid = scrctl::hid::Service::open(*device, err, verbose);
    if (hid == nullptr) {
        std::fprintf(stderr, "打开 HID 服务失败: %s\n", err.c_str());
        return 1;
    }
    hid_ptr = hid.get();
    log(t0, "HID 已连接");

    // 校准：流活着时画一笔，必须落地。没有这条，"后面几次没落地"就可能是
    // "画线本身没生效"而不是门的问题。
    shot("a0-before");
    draw("control", kControlY);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    shot("a1-control");

    // ---- 阶段 2：等设备自己把流结束掉 ----
    // 判据用"一个包都收不到"：设备死之前还会每秒发自己的 RTCP SR，所以连 SR 都
    // 停了才说明会话真的没了。
    unsigned long long prev = drainer->packets();
    auto last_change = clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const unsigned long long got = drainer->packets();
        if (got != prev) {
            prev = got;
            last_change = clock::now();
        }
        const auto quiet = static_cast<unsigned long long>(elapsed_ms(last_change));
        if (quiet >= 9000) {
            // 9 秒是"连数据报（含每秒那个 SR 心跳）都静默"的一个宽松判据：设备在起流后
            // 约 20 秒结束会话，从最后一包起算远不到 9 秒，所以走到这一支时会话必然已经
            // 死了。宁可等久一点也不要误判成死了——这条探针要的就是"死了之后注入还灵不灵"。
            log(t0, "已静默 " + std::to_string(quiet) + "ms（设备租期 20s，此刻必已拆流），"
                    "判定会话已被设备结束");
            break;
        }
    }

    // 我们这侧的会话对象还活着、HID 连接也还在——这就是自动化框架的真实处境。
    shot("b0-before");
    draw("idle-dead", kIdleDeadY);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    shot("b1-idle-dead");

    // ---- 阶段 3：我们主动把会话拆掉 ----
    drainer.reset();
    session.reset();  // 析构里发 stopmediastream
    log(t0, "已拆掉会话");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    shot("c0-before");
    draw("session-gone", kGoneY);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    shot("c1-session-gone");

    // ---- 阶段 4：重新起流，确认恢复路径 ----
    session = scrctl::media::StreamSession::start(*device, req, err, verbose);
    if (session == nullptr) {
        std::fprintf(stderr, "重起流失败: %s\n", err.c_str());
        return 1;
    }
    auto drainer2 = std::make_unique<Drainer>(*session);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    shot("d0-before");
    draw("revived", kRevivedY);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    shot("d1-revived");
    log(t0, "重起后的流共收到 " + std::to_string(drainer2->packets()) + " 个包");

    std::printf("\n比对： python3 tools/gate_diff.py %s\n", dir.c_str());
    return 0;
}
