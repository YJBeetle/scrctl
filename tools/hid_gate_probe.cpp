// 比较视频会话运行、确认自然结束、主动停止和重新起流时的触摸注入。
//
// 每条流显式请求 --lease 秒的 RTCP 空闲超时，不发送 RR；默认 20 秒只是请求值。
// 阶段 2 同时要求全部数据报静默和设备会话表确认结束，不能把“画面没变化”当作会话结束。
// 观察预算用尽后终止，不采集后续阶段，避免把未成立的前提写成触摸实验结果。
//
// 设备需停在无边记画笔界面。每个阶段在不同纵向带画线，并用独立截图服务保存前后图。
// tools/gate_diff.py 按固定图名和带位置比较；发送成功本身不证明触摸已落到屏幕上。
#include "ProbeCli.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ProbeWait.h"
#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

/// 后台持续接收数据报，避免接收队列积压，并为静默判断提供包计数。
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
    [[nodiscard]] unsigned long long packets() const { return packets_.load(std::memory_order_relaxed); }

private:
    scrctl::media::StreamSession &session_;
    std::thread worker_;
    std::atomic<bool> stopping_ { false };
    std::atomic<unsigned long long> packets_ { 0 };
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
        err = "截图回复没有 image 字节";
        return false;
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        err = "打不开 " + path;
        return false;
    }
    bool ok = std::fwrite(image->data.data(), 1, image->data.size(), f) == image->data.size();
    if (!ok) { err = "写入 " + path + " 失败: " + std::strerror(errno ? errno : EIO); }
    if (std::fclose(f) != 0) {
        if (ok) { err = "关闭 " + path + " 失败: " + std::strerror(errno ? errno : EIO); }
        ok = false;
    }
    return ok;
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
    std::string udid;
    std::string dir = "/tmp/gate";
    int lease = 20, quiet_ms = 9000, wait_ms = 45000;
    bool verbose = false, dry_run = false;
    CLI::App app("比较视频会话不同生命周期阶段的触摸注入");
    app.add_option("UDID", udid, "设备 UDID");
    app.add_option("--dir", dir, "截图目录（默认 /tmp/gate）");
    app.add_option("--lease", lease, "请求的 RTCP 空闲超时，秒（默认 20，不发送 RR）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--quiet-ms", quiet_ms, "确认结束前的数据报静默阈值，毫秒（默认 9000）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--wait-ms", wait_ms, "自然结束观察预算，毫秒（默认 45000；不含 RPC 与清理时限）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_flag("-v,--verbose", verbose, "输出协议日志");
    app.add_flag("--dry-run", dry_run, "只显示实验参数，不创建目录或连接设备");
    app.footer("需独占设备媒体服务：stopmediastream 使用 stopAll，会结束设备上的其它媒体会话。\n观察预算不包含连接、RPC 和停止媒体各自的协议时限。");
    try { app.parse(argc, argv); }
    catch (const CLI::CallForHelp &e) { return app.exit(e); }
    catch (const CLI::ParseError &e) { std::fprintf(stderr, "参数错误：%s\n", e.what()); return 2; }
    if (wait_ms <= quiet_ms) {
        std::fprintf(stderr, "--wait-ms 必须大于 --quiet-ms，才能在总时限前观察到静默\n");
        return 2;
    }
    if (dir.empty()) { std::fprintf(stderr, "--dir 不能为空\n"); return 2; }
    std::printf("实验参数：timeout=%d 秒，RR=off，quiet_ms=%d，wait_ms=%d\n", lease, quiet_ms, wait_ms);
    if (dry_run) { return 0; }
    std::error_code directory_error;
    std::filesystem::create_directories(dir, directory_error);
    if (directory_error) {
        std::fprintf(stderr, "创建截图目录 %s 失败: %s\n", dir.c_str(), directory_error.message().c_str());
        return 1;
    }

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
            return false;
        }
        log(t0, std::string("抓图 ") + name + " 用时 " +
                        std::to_string(static_cast<int>(elapsed_ms(ts))) + "ms");
        return true;
    };

    auto draw = [&](const char *name, double y) {
        std::string derr;
        if (!draw_line(*hid_ptr, xa, y, xb, y + kSlope, derr)) {
            std::fprintf(stderr, "%s 画线失败: %s\n", name, derr.c_str());
            return false;
        }
        log(t0, std::string("已发送 ") + name + " 触摸序列，是否落到屏幕需比较截图");
        return true;
    };

    // ---- 阶段 1：运行中的会话，采集触摸对照 ----
    scrctl::media::StreamSession::Request req;
    req.timeout_seconds = static_cast<uint32_t>(lease);
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

    // 第一组截图提供画线对照；触摸是否实际生效仍由截图差异确认。
    if (!shot("a0-before")) { return 1; }
    if (!draw("control", kControlY)) { return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!shot("a1-control")) { return 1; }

    // ---- 阶段 2：等待数据报静默，再确认设备已结束此会话 ----
    const auto wait_start = clock::now();
    auto wait_now = [&] { return static_cast<uint64_t>(elapsed_ms(wait_start)); };
    scrctl::probe::QuietWait wait(0, drainer->packets(), quiet_ms, wait_ms);
    bool ended = false;
    std::string status_error;
    for (;;) {
        const uint64_t now = wait_now();
        const auto result = wait.observe(now, drainer->packets());
        if (result == scrctl::probe::QuietResult::TimedOut) { break; }
        if (result == scrctl::probe::QuietResult::Quiet) {
            const auto state = scrctl::media::StreamSession::probe(*device, session->started().session_uuid,
                                                                   status_error, verbose);
            // 状态 RPC 本身有协议层时限；返回后仍需检查本实验总截止。
            if (wait.observe(wait_now(), drainer->packets()) == scrctl::probe::QuietResult::Quiet &&
                state == scrctl::media::StreamSession::ServerState::Ended) {
                ended = true;
                log(t0, "数据报静默且会话表已确认结束");
                break;
            }
        }
        const uint64_t remaining = wait.remaining(wait_now());
        if (remaining == 0) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(250, remaining)));
    }
    if (!ended) {
        std::fprintf(stderr, "%dms 观察预算内未确认数据报静默且会话已结束；不采集后续阶段。%s\n",
                     wait_ms, status_error.c_str());
        drainer.reset();
        std::string cleanup_error;
        session->stop(*device, cleanup_error, verbose);
        return 1;
    }

    // 保持本地会话对象和 HID 连接，采集确认设备端结束后的触摸。
    if (!shot("b0-before")) { return 1; }
    if (!draw("idle-dead", kIdleDeadY)) { return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!shot("b1-idle-dead")) { return 1; }

    // ---- 阶段 3：我们主动把会话拆掉 ----
    drainer.reset();
    if (!session->stop(*device, err, verbose)) {
        std::fprintf(stderr, "发送 stopmediastream 失败，不采集主动停止阶段: %s\n", err.c_str());
        return 1;
    }
    session.reset();  // 析构只释放本地对象，停止请求必须显式发送。
    log(t0, "stopmediastream 已成功，已释放本地会话对象");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    if (!shot("c0-before")) { return 1; }
    if (!draw("session-gone", kGoneY)) { return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!shot("c1-session-gone")) { return 1; }

    // ---- 阶段 4：重新起流，确认恢复路径 ----
    session = scrctl::media::StreamSession::start(*device, req, err, verbose);
    if (session == nullptr) {
        std::fprintf(stderr, "重起流失败: %s\n", err.c_str());
        return 1;
    }
    auto drainer2 = std::make_unique<Drainer>(*session);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (!shot("d0-before")) { return 1; }
    if (!draw("revived", kRevivedY)) { return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!shot("d1-revived")) { return 1; }
    log(t0, "重起后的流共收到 " + std::to_string(drainer2->packets()) + " 个包");

    drainer2.reset();
    if (!session->stop(*device, err, verbose)) { std::fprintf(stderr, "停止最终会话失败: %s\n", err.c_str()); return 1; }
    std::printf("\n截图采集完成；触摸是否落到屏幕仍需执行比对：python3 tools/gate_diff.py \"%s\"\n", dir.c_str());
    return 0;
}
