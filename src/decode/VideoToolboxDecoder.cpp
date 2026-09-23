#include "Decoder.h"

#include <CoreMedia/CMBlockBuffer.h>
#include <CoreMedia/CMFormatDescription.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <cstring>

namespace scrctl {
namespace {

void store_be16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

/// 一次提交对应一个输出槽，槽必须活得比回调久。
///
/// 为什么要有这个结构体：回调可能在 DecodeFrame 返回**之后**才跑。之前直接把
/// 栈上 `CVPixelBufferRef` 的地址传进去，函数返回后那块栈就作废，而下一次
/// decode() 以同样的调用深度进来、同一个地址又被复用，于是上一帧的图像被写进
/// 这一帧的槽里。症状是画面偶发整片噪声、且时好时坏（同一份录屏一份正常、隔
/// 40 分钟再录的那份第 1 帧就是噪声），用 libav 解同一份文件却是干净的，这才把
/// 范围收到解码器头上。
///
/// 现在每次提交后等 `WaitForAsynchronousFrames` 返回，保证所有已提交帧的回调都
/// 跑完了，槽才离开作用域——这样栈上放就够了，不需要堆分配。
struct OutputSlot {
    CVPixelBufferRef pb = nullptr;
};

void output_callback(void *ref_con, void *source_frame_ref_con, OSStatus status, VTDecodeInfoFlags,
                     CVImageBufferRef image_buffer, CMTime, CMTime) {
    (void)ref_con;
    auto *slot = static_cast<OutputSlot *>(source_frame_ref_con);
    if (slot == nullptr || status != noErr || image_buffer == nullptr) {
        return;
    }
    slot->pb = CVPixelBufferRetain(image_buffer);
}

/// NAL 长度前缀字节数。
///
/// 这里必须是 2，不是文档暗示可以选的 4。实测（macOS 26 / Apple Silicon）
/// CMVideoFormatDescriptionCreateFromHEVCParameterSets 会正确把该参数写进
/// hvcC 的 lengthSizeMinusOne（2 -> 1，4 -> 3），但 lengthSizeMinusOne=3 的
/// 解码会话对每一个样本都回 kVTVideoDecoderBadDataErr(-12909)，出帧率 0%；
/// 只有 2 字节前缀能 100% 工作。已用 create x write 的 2x2 矩阵排除是自己
/// 组装写错。
constexpr int kNalLengthSize = 2;
constexpr size_t kMaxNalSize = 0xFFFF;

class VideoToolboxDecoder final : public Decoder {
public:
    ~VideoToolboxDecoder() override { teardown(); }

    bool configure(const Nal &vps, const Nal &sps, const Nal &pps) override {
        teardown();

        const uint8_t *sets[3] = {vps.data(), sps.data(), pps.data()};
        const size_t sizes[3] = {vps.size(), sps.size(), pps.size()};

        OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            nullptr, 3, sets, sizes, kNalLengthSize, nullptr, &format_desc_);
        if (st != noErr) {
            std::fprintf(stderr, "HEVC 参数集解析失败: %d\n", static_cast<int>(st));
            return false;
        }

        // 属性字典的值必须是真正的 CFNumber——CoreVideo 按 CFNumber 解引用，
        // 直接塞裸 uint32_t* 会当场 SIGBUS。
        const uint32_t bgra = kCVPixelFormatType_32BGRA;
        CFNumberRef bgra_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bgra);
        if (bgra_num == nullptr) {
            teardown();
            return false;
        }
        const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
        const void *vals[] = {bgra_num};
        CFDictionaryRef dest = CFDictionaryCreate(nullptr, keys, vals, 1,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
        CFRelease(bgra_num);
        if (dest == nullptr) {
            teardown();
            return false;
        }

        const VTDecompressionOutputCallbackRecord cb{output_callback, nullptr};
        st = VTDecompressionSessionCreate(nullptr, format_desc_, nullptr, dest, &cb, &session_);
        CFRelease(dest);

        if (st != noErr || session_ == nullptr) {
            std::fprintf(stderr, "VideoToolbox 解码会话创建失败: %d\n", static_cast<int>(st));
            teardown();
            return false;
        }
        return true;
    }

    bool decode(const std::vector<Nal> &au, Frame &out) override {
        if (session_ == nullptr) {
            return false;
        }

        // 样本里只放 VCL NAL。参数集已在 format description（hvcC）里，
        // 再内联一份会被判为 bad data。
        std::vector<const Nal *> slices;
        slices.reserve(au.size());
        size_t total = 0;
        for (const auto &n : au) {
            if (n.size() < 2) {
                continue;
            }
            const uint8_t type = static_cast<uint8_t>((n[0] >> 1) & 0x3F);
            if (type >= 32) {
                continue;  // 非 VCL：参数集 / SEI / 保留
            }
            if (n.size() > kMaxNalSize) {
                warn_oversized(n.size());
                return false;
            }
            slices.push_back(&n);
            total += kNalLengthSize + n.size();
        }
        if (total == 0) {
            return false;
        }

        std::vector<uint8_t> buf;
        buf.reserve(total);
        // NAL 按原样拷贝，含 emulation prevention 字节：长度前缀与样本字节数必须
        // 对得上，解码器只按长度读、不会替你去 unescape（去掉了反而会让 RBSP 里
        // 冒出 00 00 01，见 AnnexB.h 里 `Nal` 的语义）。
        for (const Nal *n : slices) {
            uint8_t len[2];
            store_be16(len, static_cast<uint16_t>(n->size()));
            buf.insert(buf.end(), len, len + kNalLengthSize);
            buf.insert(buf.end(), n->begin(), n->end());
        }

        CMBlockBufferRef bb = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            nullptr, nullptr, buf.size(), kCFAllocatorDefault, nullptr, 0, buf.size(),
            kCMBlockBufferAssureMemoryNowFlag, &bb);
        if (st != kCMBlockBufferNoErr) {
            return false;
        }
        st = CMBlockBufferReplaceDataBytes(buf.data(), bb, 0, buf.size());
        if (st != kCMBlockBufferNoErr) {
            CFRelease(bb);
            return false;
        }

        CMSampleTimingInfo timing{};
        timing.duration = kCMTimeInvalid;
        timing.decodeTimeStamp = kCMTimeInvalid;
        timing.presentationTimeStamp = CMTimeMake(pts_num_++, 1000);

        const size_t sample_size = buf.size();
        CMSampleBufferRef sb = nullptr;
        st = CMSampleBufferCreateReady(nullptr, bb, format_desc_, 1, 1, &timing, 1, &sample_size,
                                       &sb);
        CFRelease(bb);
        if (st != noErr || sb == nullptr) {
            return false;
        }

        OutputSlot slot;
        st = VTDecompressionSessionDecodeFrame(session_, sb, 0, &slot, nullptr);
        CFRelease(sb);
        if (st != noErr) {
            if (slot.pb != nullptr) {
                CVPixelBufferRelease(slot.pb);
            }
            return false;
        }
        // 等所有已提交帧的回调跑完，之后才允许 slot 离开作用域。代价是不做流水
        // （一次只提交一帧，本来也不需要更深）。
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        if (slot.pb == nullptr) {
            return false;
        }
        copy_out(slot.pb, out);
        CVPixelBufferRelease(slot.pb);
        slot.pb = nullptr;
        return static_cast<bool>(out);
    }

    [[nodiscard]] const char *backend_name() const override { return "VideoToolbox"; }

private:
    void teardown() {
        if (session_ != nullptr) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_desc_ != nullptr) {
            CFRelease(format_desc_);
            format_desc_ = nullptr;
        }
    }

    /// 2 字节前缀的硬上限。真机流实测最大 NAL 约 17KB，正常不会触发；
    /// 一旦触发说明码流超出了本后端能力，需要改走软解（libav）。
    static void warn_oversized(size_t n) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "NAL 尺寸 %zu 超过 2 字节长度前缀上限，已丢弃该帧。"
                         "该码流需要软件解码后端。\n",
                         n);
        }
    }

    static void copy_out(CVPixelBufferRef pb, Frame &out) {
        if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            return;
        }
        const auto *src = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(pb));
        const size_t width = CVPixelBufferGetWidth(pb);
        const size_t rows = CVPixelBufferGetHeight(pb);
        const size_t src_pitch = CVPixelBufferGetBytesPerRow(pb);
        const size_t row_bytes = width * 4;

        if (src != nullptr && rows > 0 && row_bytes > 0) {
            out.width = static_cast<uint32_t>(width);
            out.height = static_cast<uint32_t>(rows);
            out.row_pitch = static_cast<uint32_t>(row_bytes);
            out.bytes_per_pixel = 4;
            out.pixels.resize(row_bytes * rows);
            for (size_t r = 0; r < rows; ++r) {
                std::memcpy(out.pixels.data() + r * row_bytes, src + r * src_pitch, row_bytes);
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    }

    CMFormatDescriptionRef format_desc_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    int64_t pts_num_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> create_platform_decoder() {
    return std::make_unique<VideoToolboxDecoder>();
}

}  // namespace scrctl
