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
/// 它**不**兼任触摸的认证门：早先以为"没有流在跑设备就把 HID 面标成未认证、输入
/// 会被静默丢掉"，2026-09-25 复测推翻了这个结论（见 docs §11 与
/// `tools/hid_gate_probe`）。这个对象只负责提供帧。
class FramePump {
public:
    struct Options {
        uint32_t display_id = 1;
        /// 非空则顺手把 Annex-B 码流录到该文件。
        std::string record_path;
        /// 断流之后隔多久没等到关键帧就重起会话；0 = 从不重起。
        ///
        /// 这里原先的理由（"这条流不周期发 IDR，RTCP PLI 实测设备也不理（docs §13），
        /// 所以重起是唯一能让画面重新自洽的手段"）**前半句仍然成立、后半句作废**：
        /// "PLI 设备不理"那批实验发出去的 UDP 一个都没到设备（`build_udp_datagram` 的
        /// 拼装 bug），所以它测的是"什么都没发"。设备现在到底理不理 PLI/FIR 属于
        /// **重新待测**，在那之前重起仍是拿回干净画面的手段，这一位照旧留着。
        int stall_restart_ms = 2000;
        /// **完全收不到包**多久就认死重起会话；0 = 不检查。
        ///
        /// 这条是补 stall_restart_ms 的盲区：它要求"序号有缺口"才触发，可设备把
        /// 流结束掉的时候是一个包都不发（实测：进程活着、三个线程都在等，而设备侧
        /// getmediastreamserverstatus 已经报 running:false）。只按缺口判断的话，
        /// 这种最常见的死法永远检不出来，用户看到的就是"窗口冻住了"。
        ///
        /// 它是"不再问、直接重起"那一档。比它低的地方还有一档：静默一超过一个 SR
        /// 周期（1.2s）就去问一句设备，答"结束了"立刻重起——所以实际救流的时刻
        /// 是 ~1.2s 而不是这个数，这个值只是"问也不问了"的上限。判据和催流那条路
        /// 完全共用一把尺（见 FramePump.cpp 的 kSrPeriodMs）。
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
        /// 非视频载荷（设备的 RTCP SR）被跳过的包数。
        uint64_t other_payload = 0;
        /// 泵自己按数据报开头分的两类计数：SR 是 0x81 0xc8 那个每秒心跳，video 是
        /// 其余（RTP 视频）。和 `packets` 的关系是 video + sr == packets，和
        /// `other_payload` 的关系是它由拆包器按 payload type 判、这两个由收包处按
        /// 字节判 —— 两把尺不一致时（画面静止、decoded 不涨，却在"视频包"上一直有
        /// 进账）就是这里的读数在说话，而不是"设备大概不会这么发"。
        uint64_t sr_packets = 0;
        uint64_t video_packets = 0;
        /// 我们**发出去**的续命 RR 个数与发送失败次数。这一对是判活的直接读数：
        /// 会话到期而 rtcp_sent 还在涨，说明"包发了但设备没收到"（那是数据报本身的
        /// 问题，实测曾经如此——见 docs §13 那个长度字段/校验和 bug），
        /// rtcp_sent 不涨才是泵停了。
        uint64_t rtcp_sent = 0;
        uint64_t rtcp_failed = 0;
        /// 发出去的 PLI（关键帧请求）个数。它和 `dropped_awaiting_keyframe` 一起讲完
        /// 一次"丢帧 -> 要 IDR -> 恢复"的故事：只涨前者不涨后者的话，PLI 就没起作用。
        uint64_t pli_sent = 0;
        /// 本会话开始时 `packets` 的快照，用来把我们的累计数搬到设备那条数轴上。
        /// 设备 SR 里的累计包数**每条会话从零重数**，而 packets 跨会话连着涨：不扣
        /// 基线的话重起一次之后就是"设备 143 / 我 5866"，看着像丢了五千包。
        /// 所以和 dev_sent_packets 比的永远是 packets - session_packets_base。
        uint64_t session_packets_base = 0;
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
    /// 为什么需要：这条流的会话有一条**我们自己在请求里报的**租期（`kSessionLeaseSeconds`，
    /// 现在报 20 秒并按秒回 RR 续命；docs §13 记着这个数怎么来的——它一度被当成设备固定
    /// 的、RTCP 续不上的 20 秒，为此白找了很久），而流一旦停了，设备上任何变化都不会再
    /// 推过来。泵自己按"静默满 silence_restart_ms"发现这件事，那是给"有人盯着窗口"的场景
    /// 兜底用的；交互和自动化要的是**动手/取帧的这一刻**就知道，所以用这条便宜的信号去催
    /// 一次检查。判断本身分三档，靠的是设备那条每秒一个的 RTCP SR 心跳：心跳还在就什么都
    /// 不做，可疑区间去问设备一句，静默到两个心跳以上直接重起（推导见 FramePump.cpp 里
    /// kSrPeriodMs 那段）。流还新鲜时这是个空操作，所以每次截图、每次按键都催得起。
    void wake() { wake_requested_ = true; }

    /// 泵现在是不是正在救这条流：从"决定要去问设备/重起"开始，到新会话的第一帧交
    /// 出来为止。
    ///
    /// 取帧方需要它，因为"等不到新帧"有两种完全不同的原因：一种是重起还在路上
    /// （再等一会儿就有帧），另一种是屏幕本来就静止（再等多久都没有，而"最新一帧"
    /// 就是当前画面）。只看"有没有新帧"分不出这两种，用同一个超时去等必然一边太短
    /// 一边太长。
    ///
    /// 覆盖范围必须从**问设备那一句**开始，不能只盖住重起本身：问一句要 100~300ms，
    /// 只置在 restart() 里的话，取帧方在这段窗口里读到 false，照样把旧帧交出去
    /// （实测三次全这样）。
    [[nodiscard]] bool reviving() const { return reviving_; }

    /// 视频这条路是不是已经**确认走不通**：连续几次重起都因为"单帧超过解码后端上限"
    /// 而一帧都没解出来。这不是等一次新关键帧就能好的事——画面本身太复杂（实测无边记
    /// 画满白线的看板，IDR 256278 字节，重起十次还是同一个尺寸），而 VideoToolbox 只吃
    /// 2 字节长度前缀。置起之后泵改成很偶尔才试一次（画面变简单时要能自己回来），
    /// 调用方则应该改走别的取图方式（控制单元走截图服务），别再为等帧花预算。
    /// 解出任意一帧就自动清掉。
    [[nodiscard]] bool video_unusable() const { return video_unusable_; }

    /// 等到帧号大于 `since` 再取。返回该帧的帧号，0 表示超时。
    ///
    /// **`since` 要用调用这一刻的 serial()，不要用"我上次取走的那一帧的号"。**
    /// 看着等价，其实不等价：设备会把空闲的会话结束掉（见 reviving() 那段），而会话
    /// 一死，泵手里那张"最新"的帧就永远停在拆流前那一刻。这时"比我上次取走的号大"
    /// 这张旧帧也满足——于是它在 1~3ms 内被当成新帧交出去，调用方以为看到了刚刚动作
    /// 之后的画面（实测就是这个现象：截图耗时 3ms，内容与动作前逐像素一致）。
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
    /// 不 spawn 线程——线程只由 `start()` 起，为的是让"worker 会读的无锁字段"
    /// 都能在线程开始跑之前写完（推导见 `start()` 的注释）。
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
    /// 救流进行中：决定去问设备/重起时置位，新会话第一帧交出来时清掉。
    std::atomic<bool> reviving_ { false };
    bool stopping_ = false;
    int width_ = 0;
    int height_ = 0;
    Stats stats_;
    /// 最后一个**收到的数据报**的时刻。设备结束流时是一个包都不发，只看序号缺口
    /// 检不出来。
    uint64_t last_packet_ms_ = 0;
    /// 下一个续命 RR 该在什么时候发出去（见 FramePump.cpp 的 kRtcpPeriodMs）。
    /// 设备的 `RTCPTimeoutInterval` 是"距离上次收到我们 RTCP 多久"，不是起流后的固定
    /// 到期——所以这一位是这条流能活过 20 秒的原因，不是锦上添花。
    uint64_t next_rtcp_ms_ = 0;
    /// 最早什么时候可以再发一个 PLI（0 = 立刻可发）。见 FramePump.cpp 的 request_keyframe。
    uint64_t next_pli_ms_ = 0;
    /// 上一次发 PLI 的时刻（等待期间每秒会被刷新，所以**不能**拿它算等待期限）。
    uint64_t last_pli_ms_ = 0;
    /// 这一轮"等一个干净 IDR"的起点：第一次发 PLI 的时刻，拿到关键帧就清零。
    /// 后备重起按它算——用 `last_pli_ms_` 的话每秒重发会把它一直往前推，判据永不成立。
    uint64_t first_pli_ms_ = 0;
    /// 当前"等关键帧"的成因是不是丢包。只有它才走后备重起；超大 NAL 那种成因有自己的
    /// 上限与降级，不能被通用判据抢走（见 FramePump.cpp 的 stalled 那段）。
    bool awaiting_idr_from_loss_ = false;
    /// 最后一个**解出来的关键帧**的时刻。后备重起拿它做"别在刚解出关键帧时重起"这一档，
    /// 而这条流实测 30 秒才自发一个 IDR，所以它几乎恒真、不是主判据（见 stalled 那段）。
    uint64_t last_keyframe_ms_ = 0;
    /// 有超大 NAL 被丢、需要重起会话。由 AU 回调置位，收包线程消费。
    std::atomic<bool> oversized_restart_ { false };
    /// 用户刚刚动过（见 wake()）。由收包线程取走并做一次"流还活着吗"的检查。
    std::atomic<bool> wake_requested_ { false };
    uint64_t last_restart_ms_ = 0;
    /// 因为"单帧超过后端上限"而重起过几次了。解出一帧就清零；到顶之后改成很偶尔
    /// 试一次（见 kMaxOversizedRestarts）。
    int oversized_restarts_ = 0;
    std::atomic<bool> video_unusable_ { false };
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
