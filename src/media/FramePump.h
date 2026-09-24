#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decode/Decoder.h"
#include "media/MediaOffer.h"
#include "rt/RtpHevc.h"

namespace scrctl::media {

class StreamSession;
}

namespace scrctl::remote {
class Device;
}

namespace scrctl::media {

/// 把"起流 -> 收包 -> 拆 AU -> 解码"整条链放进一个后台线程，随时能取到最新一帧。
///
/// 为什么要有它：`screencaptureservice` 抓一张要 344~613ms，而视频流是 60fps——
/// 也就是说"看现在屏幕上是什么"这件事，走流比走截图服务快一个数量级。对镜像和
/// 对自动化框架（截图 -> 识别 -> 动作 的循环）这都是决定性的差别。
///
/// 另外它兼任触摸注入的认证门：设备只在有媒体会话在跑时才把 HID 面标成已认证，
/// 所以这个对象活着的时候输入才可用。
class FramePump {
public:
    struct Options {
        uint32_t display_id = 1;
        /// 非空则顺手把 Annex-B 码流录到该文件。
        std::string record_path;
        /// 断流之后隔多久没等到关键帧就重起会话；0 = 从不重起。
        /// 这条流不周期发 IDR，RTCP PLI 实测设备也不理（docs §13），所以重起是
        /// 唯一能让画面重新自洽的手段。
        int stall_restart_ms = 2000;
        /// **完全收不到包**多久就重起会话；0 = 不检查。
        ///
        /// 这条是补 stall_restart_ms 的盲区：它要求"序号有缺口"才触发，可设备把
        /// 流结束掉的时候是一个包都不发（实测：进程活着、三个线程都在等，而设备侧
        /// getmediastreamserverstatus 已经报 running:false）。只按缺口判断的话，
        /// 这种最常见的死法永远检不出来，用户看到的就是"窗口冻住了"。
        int silence_restart_ms = 3000;
        /// 起流时用的 offer（码率、能力串都在这里）。每次重起沿用同一份。
        Offer offer;
        /// 用平台硬件后端（macOS 上是 VideoToolbox），而不是默认的软件解码。
        ///
        /// **默认是软解**，理由是能力而不是速度：VideoToolbox 只吃 2 字节的 NAL
        /// 长度前缀，而这条真机流的单帧能到 256278 字节（转场动画里的一个 P 帧），
        /// 装不下就只能整帧丢 + 重起会话，用户看到的是一次卡顿或花屏。实测同一
        /// 600 帧：软解 CPU 6.7 秒、硬解 1.0 秒，但**两边墙钟几乎一样**（24.7s vs
        /// 26.1s）——瓶颈是包到达速率不是解码，所以软解多花的 CPU 不换来任何延迟
        /// 收益。想要那 1 秒 CPU 就开这个开关，代价是超大帧会退化成丢帧 + 重起。
        bool use_hardware = false;
    };

    struct Stats {
        uint64_t packets = 0;
        uint64_t decoded = 0;
        /// 解了但没出图的 AU 数（参考帧未就绪，或丢了分片）。
        uint64_t no_output = 0;
        uint64_t gaps = 0;
        uint64_t restarts = 0;
        /// 因解码器不可用而丢弃的帧数（取帧方跟不上时不阻塞收包线程）。
        uint64_t dropped = 0;
        /// 单个 NAL 超过 2 字节长度前缀上限（65535）而被整帧丢掉的次数。
        /// 这条流没有周期 IDR，丢一帧参考链就永久坏，所以它同时是重起的触发器。
        uint64_t dropped_oversized = 0;
        /// 因为"还没等到干净关键帧"（need_keyframe_）而被挡掉的 AU 数。这个数才
        /// 是"包都收到了、画面却只有 12 帧"的真正账：它之前完全没有计数。
        uint64_t dropped_awaiting_keyframe = 0;
        /// 拆包器累计的"分片没收完就作废"次数。它和 seq_gaps 一起构成
        /// need_keyframe_ 的触发条件，之前只打了 gaps。
        uint64_t dropped_fragments = 0;
        /// AU 切分器一共交出来多少个 AU（和 decoded 一比就知道丢在哪一层）。
        uint64_t aus = 0;
        /// 设备在它自己的 RTCP SR 里报的累计已发视频包数/字节数（最后一次看到的值）。
        /// 这是**唯一不经过我们链路的读数**：拿它和 packets 一比，就能分清
        /// "设备只编这么点"和"设备发了但我们没收全"，而且是在同一条会话里比。
        uint64_t dev_sent_packets = 0;
        uint64_t dev_sent_octets = 0;
        /// 非视频载荷（设备的 RTCP SR）被跳过的包数。它同时用来回答"我们到底有没有
        /// 收到 SR"——scrctl 的会话里这个数一直是 0，而探针每秒都收到一个。
        uint64_t other_payload = 0;
        /// 每一段各花了多少毫秒（累计）。12fps 却烧掉 1.5 个核，那 120ms/帧 必须
        /// 有个归属：收包、拆包、切 AU、解码、还是把帧交给取帧方。
        double ms_depacketize = 0, ms_decode = 0, ms_publish = 0;
        uint64_t decode_calls = 0;
    };

    /// 在已经建好的会话上起泵。失败时 err 带设备的人话。
    static std::unique_ptr<FramePump> start(scrctl::remote::Device &device, const Options &options,
                                            std::string &err, bool verbose = false);

    ~FramePump();

    FramePump(const FramePump &) = delete;
    FramePump &operator=(const FramePump &) = delete;

    /// 取最新一帧（可能是上一帧的重复副本）。timeout_ms 内一帧都没有则返回 false。
    bool latest(Frame &out, int timeout_ms);

    /// 告诉泵"用户刚刚动了"。
    ///
    /// 为什么需要：静止画面上设备会主动把整条流结束掉（实测约 3 秒），而泵是按
    /// "静默满 3 秒"才发现这件事并重起的。于是用户的手感是"点下去要愣一下画面才
    /// 动"。交互本身就是"接下来一定会有画面变化"的信号，用它去催一次检查，比等
    /// 静默窗口到点准得多，也便宜得多（没人操作时一次 RPC 都不发）。
    void wake() { wake_requested_ = true; }

    /// 等到帧号大于 `since` 再取。用来保证"拿到的一定比我上次看的新的"。
    /// 返回该帧的帧号，0 表示超时。
    uint64_t newer(Frame &out, uint64_t since, int timeout_ms);

    [[nodiscard]] uint64_t serial() const;
    [[nodiscard]] Stats stats() const;
    /// 协商到的视频 payload type，HID 之外的调试用得上。
    [[nodiscard]] uint8_t payload_type() const;
    [[nodiscard]] uint16_t receiver_port() const;
    /// 最近一帧的尺寸（还没出帧时为 0）。
    void size(int &width, int &height) const;

private:
    FramePump(scrctl::remote::Device &device, Options options, bool verbose);
    void loop();
    /// 停旧会话、起新会话。第一次调用（起流）与重起共用同一条路径。
    bool restart(std::string &err);

    scrctl::remote::Device &device_;
    Options options_;
    bool verbose_ = false;

    std::unique_ptr<StreamSession> session_;
    std::thread worker_;
    bool worker_running_ = false;
    FILE *record_ = nullptr;

    /// 只属于后台线程：每个会话一套，重起流时整个换掉。
    std::unique_ptr<AnnexBParser> parser_;

    /// 后台线程与取帧方之间唯一共享的就是下面这些，其余状态只属于那个线程。
    /// 取帧方读的尺寸是快照，免得为了拿个宽高就要拷一帧像素。
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Frame frame_;
    /// 解码目标。发布时和 frame_ 交换，所以它的容量在第二帧之后就一直复用着。
    Frame publishing_;
    uint64_t serial_ = 0;
    bool stopping_ = false;
    int width_ = 0;
    int height_ = 0;
    Stats stats_;
    uint64_t last_keyframe_ms_ = 0;
    uint64_t gaps_at_last_check_ = 0;
    /// 最后一个**收到的数据报**的时刻。设备结束流时是一个包都不发，只看序号缺口
    /// 检不出来。
    uint64_t last_packet_ms_ = 0;
    /// 有超大 NAL 被丢、需要重起会话。由 AU 回调置位，收包线程消费。
    std::atomic<bool> oversized_restart_ { false };
    /// 用户刚刚动过（见 wake()）。由收包线程取走并做一次"流还活着吗"的检查。
    std::atomic<bool> wake_requested_ { false };
    uint64_t last_restart_ms_ = 0;
    /// 本会话有没有解出过关键帧。开头那个 IDR 若被丢掉（比如它正好超大），
    /// 后面所有帧都对着空参考解成一片灰，且再不会自愈。
    std::atomic<bool> ever_keyframe_ { false };
    uint64_t session_start_ms_ = 0;
    int nokey_restarts_ = 0;
    /// 丢过包之后必须等一个**完整**的关键帧才继续解，否则残缺关键帧会把参考链
    /// 永久带坏（用户症状：先正常 -> 卡几秒 -> 之后一直花）。
    std::atomic<bool> need_keyframe_ { false };
    uint64_t loss_seen_ = 0;
};

/// 编码帧里"真正显示出来"的那一块。
struct DisplayCrop {
    int x = 0, y = 0, w = 0, h = 0;
};

/// 设备编码分辨率比逻辑显示大（HEVC 按 CTU 对齐填充），多出来的是垃圾像素。
/// 实测 iPhone 13 mini：编码 1136x2464，逻辑显示 1125x2436，右 11px / 底 28px。
///
/// 不裁会有两个后果：画面边缘有一条噪声，以及**触摸坐标偏**——触摸面的 0..1 是
/// 相对逻辑显示的，用编码尺寸当分母会在右下方向错出 1% 左右。
///
/// 正解是读 SPS 的 conformance window，那样任何机型都不用列数字；在做到那步之前，
/// 至少让"猜出来的这对数字"只存在于一个函数里、被一个测试钉住，而不是散在
/// 应用和控制器两边。
[[nodiscard]] DisplayCrop display_crop(int coded_w, int coded_h);

}  // namespace scrctl::media
