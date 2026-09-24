#include "decode/Decoder.h"

// libav 的头是 C 头，自己不带 extern "C"，不包一层就会按 C++ 原型去找符号，
// 链接时报 "symbol not found"（而 dylib 里明明有）。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstring>

namespace scrctl {
namespace {

/// libav 的错误码是人话，但要先转一次才看得到。
std::string av_strerr(int st) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(st, buf, sizeof(buf));
    return buf;
}

/// 软解后端（libavcodec）。
///
/// 为什么必须有它：VideoToolbox 只接受 2 字节的 NAL 长度前缀，而这条真机流的
/// 关键帧能长到 70101 字节（主屏壁纸实测）。开头那个 IDR 一丢，因为流不周期发
/// IDR，画面就永久灰掉——平台后端在这条码流面前是**能力不足**，不是调参能解决的。
///
/// 另外 Linux/Windows 上没有 VideoToolbox，这个后端就是唯一的出路。
class FFmpegDecoder final : public Decoder {
public:
    ~FFmpegDecoder() override { teardown(); }

    bool configure(const Nal &vps, const Nal &sps, const Nal &pps) override {
        teardown();

        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        if (codec == nullptr) {
            std::fprintf(stderr, "libavcodec 里没有 HEVC 解码器\n");
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (ctx_ == nullptr) {
            return false;
        }
        // 逐帧出图，不要解码器攒帧。这条流 has_b_frames=0（ffprobe 实测），
        // 攒帧只会白加延迟——镜像里延迟就是手感。
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        // 多线程默认就行：软解 2.8 兆像素的帧，单线程只有十几 fps。
        ctx_->thread_count = 0;
        // 参数集另外存一份，跟在没带参数集的 AU 前面一起送进去（见 decode()），
        // 所以要在这之前把 open 做完——open 之后才允许 send_packet。
        const int open_st = avcodec_open2(ctx_, codec, nullptr);
        if (open_st < 0) {
            std::fprintf(stderr, "libavcodec 打开 HEVC 解码器失败: %s\n",
                         av_strerr(open_st).c_str());
            teardown();
            return false;
        }

        // 参数集另外存一份，跟在**每个** AU 前面一起送进去（见 decode()）。
        sets_.clear();
        append_nal(sets_, vps);
        append_nal(sets_, sps);
        append_nal(sets_, pps);

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            teardown();
            return false;
        }
        return true;
    }

    bool decode(const std::vector<Nal> &au, Frame &out) override {
        if (ctx_ == nullptr) {
            return false;
        }
        std::vector<uint8_t> annexb;
        if (!has_parameter_sets(au)) {
            annexb = sets_;  // 这个 AU 不带参数集，补上建会话时那份
        }
        for (const auto &n : au) {
            append_nal(annexb, n);
        }
        if (annexb.empty()) {
            return false;  // 一个 NAL 都没有
        }

        AVPacket *pkt = av_packet_alloc();
        if (pkt == nullptr) {
            return false;
        }
        // 必须走 av_new_packet：libav 要求输入缓冲后面有 AV_INPUT_BUFFER_PADDING_SIZE
        // 的零填充，位读取器会读到填充区去。直接指着自己的 vector 是未定义行为。
        if (av_new_packet(pkt, static_cast<int>(annexb.size())) < 0) {
            av_packet_free(&pkt);
            return false;
        }
        std::memcpy(pkt->data, annexb.data(), annexb.size());

        const int send_st = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (send_st < 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr, "libavcodec 收包失败: %s\n", av_strerr(send_st).c_str());
            }
            return false;
        }

        av_frame_unref(frame_);
        const int st = avcodec_receive_frame(ctx_, frame_);
        if (st < 0) {
            return false;  // EAGAIN：攒着，下一个 AU 再出
        }
        return to_frame(frame_, out);
    }

    [[nodiscard]] const char *backend_name() const override { return "libavcodec/hevc"; }

private:
    static bool has_parameter_sets(const std::vector<Nal> &au) {
        for (const auto &n : au) {
            if (n.size() >= 2) {
                const int type = (n[0] >> 1) & 0x3F;
                if (type == 32 || type == 33 || type == 34) {  // VPS / SPS / PPS
                    return true;
                }
            }
        }
        return false;
    }

    /// 参数集要跟在"没带参数集的 AU"前面送：建会话那次只 configure 了参数集、
    /// 没送载荷，而**下一次被喂的 AU 有可能不带参数集**（非关键帧就是，实测每
    /// 400 帧才有一个关键帧）。AU 自己带了就照原样送——那才是当前有效的那份，
    /// 拿旧 SPS 去覆盖会解错。
    static void append_nal(std::vector<uint8_t> &out, const Nal &n) {
        const uint8_t sc[4] = {0, 0, 0, 1};
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), n.begin(), n.end());
    }

    bool to_frame(const AVFrame *f, Frame &out) {
        const int w = f->width;
        const int h = f->height;
        const AVPixelFormat src = static_cast<AVPixelFormat>(f->format);
        const AVPixFmtDescriptor *desc = src == AV_PIX_FMT_NONE ? nullptr
                                                                : av_pix_fmt_desc_get(src);
        if (w <= 0 || h <= 0 || desc == nullptr ||
            (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            std::fprintf(stderr, "软解后端只认 CPU 帧，收到 format=%d %dx%d\n",
                         static_cast<int>(src), w, h);
            return false;
        }

        const auto row_bytes = static_cast<uint32_t>(w) * 4;
        out.pixels.assign(static_cast<size_t>(row_bytes) * static_cast<size_t>(h), 0);
        out.width = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.row_pitch = row_bytes;
        out.bytes_per_pixel = 4;

        uint8_t *dst_data[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
        const int dst_linesize[4] = {static_cast<int>(row_bytes), 0, 0, 0};
        sws_ = sws_getCachedContext(sws_, w, h, src, w, h, AV_PIX_FMT_BGRA, SWS_POINT,
                                    nullptr, nullptr, nullptr);
        if (sws_ == nullptr ||
            sws_scale(sws_, f->data, f->linesize, 0, h, dst_data, dst_linesize) <= 0) {
            out = Frame {};
            return false;
        }
        return true;
    }

    void teardown() {
        if (sws_ != nullptr) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (ctx_ != nullptr) {
            avcodec_free_context(&ctx_);
            ctx_ = nullptr;
        }
    }

    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    SwsContext *sws_ = nullptr;
    std::vector<uint8_t> sets_;
};

}  // namespace

std::unique_ptr<Decoder> create_software_decoder() {
    // 这条流的码流标志是"全范围 420p"，libav 现在仍按废弃像素格式 yuvj420p 报出来，
    // swscale 每帧为此刷一行 "deprecated pixel format used"。它按全范围处理是**对的**
    // （换掉就得自己搬 range 细节，反而容易错），所以只把它的日志级别压到 error：
    // 本文件的诊断一律走 fprintf，不受影响。
    static bool log_quieted = false;
    if (!log_quieted) {
        av_log_set_level(AV_LOG_ERROR);
        log_quieted = true;
    }
    return std::make_unique<FFmpegDecoder>();
}

}  // namespace scrctl
