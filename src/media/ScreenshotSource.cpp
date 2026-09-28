#include "media/ScreenshotSource.h"

#include <chrono>
#include <utility>

#include "remote/Device.h"
#include "remote/Rsd.h"
#include "xpc/XpcValue.h"

#ifdef SCRCTL_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace scrctl::media {
namespace {

constexpr const char *kService = "com.apple.coredevice.screencaptureservice";
constexpr const char *kFeature = "com.apple.coredevice.feature.capturescreenshot";
constexpr const char *kAction = "com.apple.coredevice.action.capturescreenshot";

/// 截图失败后的退避。不能贴着失败猛重试：设备拒一次（比如屏幕睡了的某些状态）
/// 就是一句语义错误，猛重试只会把日志刷满而不会变好。
constexpr int kRetryBackoffMs = 250;
/// 单次截图 RPC 的上限。实测 0.2~0.6 秒（MaaFW 侧静态 84ms），5 秒已是 8~25 倍余量；
/// 这个数同时是"切换画面源时渲染线程最多冻多久"与"teardown join 最多等多久"的上限
/// （审查 P3）。原先的 30 秒换来的只是"设备真卡死时晚 25 秒放弃"，而那一档本来也由
/// worker 的退避重试兜着。
constexpr int kCaptureTimeoutMs = 5000;

}  // namespace

bool decode_png_bgra(const std::vector<uint8_t> &png, scrctl::Frame &out, std::string &err) {
#ifdef SCRCTL_HAVE_LIBAV
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        err = "这个 libav 里没有 PNG 解码器";
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    if (ctx != nullptr && pkt != nullptr && frame != nullptr) {
        pkt->data = const_cast<uint8_t *>(png.data());
        pkt->size = static_cast<int>(png.size());
        if (avcodec_open2(ctx, codec, nullptr) == 0 && avcodec_send_packet(ctx, pkt) == 0 &&
            avcodec_receive_frame(ctx, frame) == 0) {
            const int w = frame->width;
            const int h = frame->height;
            // 不白名单像素格式：设备回的 PNG 随画面内容变（纯色屏与画布屏解出来的
            // format 就不一样，2026-09-28 真机撞过一次），一律 sws 转到 BGRA。
            // libswscale 与 libavcodec 同在链接面里（CMake 的 pkg_check_modules 一起要的）。
            if (w > 0 && h > 0) {
                SwsContext *sws = sws_getContext(w, h, static_cast<AVPixelFormat>(frame->format), w,
                                                 h, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr,
                                                 nullptr);
                if (sws != nullptr) {
                    out.width = static_cast<uint32_t>(w);
                    out.height = static_cast<uint32_t>(h);
                    out.row_pitch = static_cast<uint32_t>(w) * 4;
                    out.bytes_per_pixel = 4;
                    out.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                    uint8_t *dst = out.pixels.data();
                    int dst_linesize = w * 4;
                    sws_scale(sws, frame->data, frame->linesize, 0, h, &dst, &dst_linesize);
                    sws_freeContext(sws);
                    ok = true;
                } else {
                    err = "sws_getContext 建不了到 BGRA 的转换（format=" +
                          std::string(av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))) +
                          "）";
                }
            } else {
                err = "PNG 解出来的尺寸不是正数";
            }
        } else {
            err = "libav 解这张 PNG 失败（字节可能不是 PNG）";
        }
    } else {
        err = "libav 分配解码上下文失败";
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ok;
#else
    (void)png;
    (void)out;
    err = "这个构建没编 libav（-DSCRCTL_LIBAV=OFF），兜底镜像没有 PNG 解码后端";
    return false;
#endif
}

ScreenshotSource::ScreenshotSource(remote::Device &device) : device_(device) {}

ScreenshotSource::~ScreenshotSource() { stop(); }

std::unique_ptr<ScreenshotSource> ScreenshotSource::start(remote::Device &device,
                                                          std::string &err, bool capture_first) {
    if (!device.rsd().has_service(kService)) {
        err = "设备目录里没有 " + std::string(kService);
        return nullptr;
    }
    std::unique_ptr<ScreenshotSource> src(new ScreenshotSource(device));
    // 第一张同步拿：兜底路如果连一张都拿不到，它就是空的，原因要直接交出去，
    // 而不是起一个线程在里面默默失败。运行中降级那一路传 capture_first=false：那次
    // 切换发生在渲染线程上，同步拿会把窗口冻到 kCaptureTimeoutMs（审查 P3）。
    if (capture_first) {
        std::vector<uint8_t> png;
        if (!src->capture_once(png, err)) {
            return nullptr;
        }
        scrctl::Frame first;
        if (!decode_png_bgra(png, first, err)) {
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(src->mu_);
            src->frame_ = std::move(first);
            src->serial_ = 1;
            src->stats_.frames = 1;
            src->stats_.bytes = png.size();
        }
    }
    src->worker_ = std::thread([raw = src.get()] { raw->loop(); });
    return src;
}

bool ScreenshotSource::capture_once(std::vector<uint8_t> &png, std::string &err) {
    // 每轮一条新连接：这条服务一条连接只服务一次请求（复用同一条时第二张开始全失败，
    // 2026-09-28 真机量到）。建连接的开销算在实测那 0.5 秒里，不是额外代价。
    auto conn = device_.connect(kService, err, false);
    if (conn == nullptr) {
        return false;
    }
    auto input = xpc::make_dict();
    xpc::dict_set(input, "displayUniqueID", xpc::make_null());
    xpc::dict_set(input, "requestedFormat", xpc::make_string("png"));
    xpc::Value out;
    // 30 秒上限：截图 RPC 实测半秒级，给到 30 秒是防"设备某次卡住"把泵线程永久挂住；
    // 真卡到那份上这一路本来也没救了，下一轮退避后重试。
    if (conn->invoke(kFeature, kAction, input, out, kCaptureTimeoutMs, err) !=
        remote::CallResult::Ok) {
        return false;
    }
    const auto *image = out.find("image");
    if (image == nullptr || image->data.empty()) {
        err = "截图回信里没有 image 字节";
        return false;
    }
    png = image->data;
    return true;
}

void ScreenshotSource::loop() {
    while (!stopping_) {
        std::vector<uint8_t> png;
        std::string err;
        if (!capture_once(png, err)) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++stats_.failures;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBackoffMs));
            continue;
        }
        scrctl::Frame f;
        if (!decode_png_bgra(png, f, err)) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++stats_.failures;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBackoffMs));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            frame_ = std::move(f);
            ++serial_;
            ++stats_.frames;
            stats_.bytes += png.size();
        }
        cv_.notify_all();
    }
}

bool ScreenshotSource::latest(scrctl::Frame &out, uint64_t &serial, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    const uint64_t want = serial;
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                 [&] { return serial_ > want || stopping_; });
    if (serial_ <= want) {
        return false;
    }
    out = frame_;
    serial = serial_;
    return true;
}

void ScreenshotSource::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

ScreenshotSource::Stats ScreenshotSource::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

void ScreenshotSource::image_size(int &w, int &h) const {
    std::lock_guard<std::mutex> lk(mu_);
    w = static_cast<int>(frame_.width);
    h = static_cast<int>(frame_.height);
}

}  // namespace scrctl::media
