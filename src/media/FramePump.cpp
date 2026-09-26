#include "media/FramePump.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "bitstream/AnnexB.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/Rtcp.h"

namespace scrctl::media {
namespace {

/// 设备的 RTCP SR 周期：流活着的时候每秒一个，画面完全静止也照发。它是"这条流还
/// 活着"的心跳——也是"静默多久算可疑"这把尺的来源。
/// 实测（docs §13 的时间轴）：视频包 13.1s 停，SR 照旧 14–19s 每秒一个，20.0s 会话
/// 从表里消失的**同一秒** SR 才停。所以静默是从会话死亡那一刻开始计时的。
constexpr uint64_t kSrPeriodMs = 1000;
/// 静默超过一个 SR 周期加一点余量：可疑，但一个 UDP 丢包就能造出同样的现象，
/// 分不清"流死了"和"SR 丢了"，所以这条只能问设备（一条 100~300ms 的状态 RPC）。
constexpr uint64_t kQuietSuspiciousMs = kSrPeriodMs + 200;
/// 静默到两个 SR 周期以上：连着丢两个心跳的概率低到不值得为它花一条 RPC，
/// 直接重起（37~90ms）。
constexpr uint64_t kQuietCertainMs = 2 * kSrPeriodMs + 500;
/// 连续几次"因为单帧太大而重起"之后就不再按节拍重试。理由：这种失败不是偶发的，
/// 是画面本身复杂到后端吃不下（无边记画满白线的看板，IDR 256278 字节），重起十次
/// 拿回来的还是同一个尺寸。以前只限频（1.5 秒一次），于是设备只要停在这种画面上，
/// 我们就每 1.5 秒做一次停+起、永不成功、一帧也拿不到，而手机白烧电与带宽。
constexpr int kMaxOversizedRestarts = 3;
/// 到顶之后的重试间隔。不是彻底停手：用户把画面弄简单了（关掉看板、回到主屏）之后
/// 这条流还得能自己活回来，所以偶尔还得试一次。解出一帧之后计数清零，回到正常节奏。
constexpr uint64_t kOversizedRetryMs = 60000;
/// 一条媒体会话的租期有多长。**这个数是我们自己在 startmediastream 请求里报的**（那个
/// 键就叫 `timeout`），设备把它原样抄进 answer 的 `RTCPTimeoutInterval`，然后从"上次
/// 收到我们 RTCP"起倒数，到点就把这条会话从设备表里摘掉。
///
/// **它不是硬到期，是空闲计时器**——这一条是 2026-09-27 才定下来的，之前一整节文档都
/// 把它当成"起流后 N 秒必死、回 RTCP 也续不上"，那个结论错在**我们发出去的 UDP 数据报
/// 从来没到过设备**（`build_udp_datagram` 的拼装 bug，见 docs §13）。同一台设备、同一个
/// 探针、只改数据报拼装，前后对照：
///
/// | 我们做什么 | 设备侧 socket `pkts in` | 结果 |
/// | --- | --- | --- |
/// | 什么都不发 | 0 | +19.97s / +19.99s 死（两次） |
/// | 每秒一个 RR | **41** | 40.2s 还在（两次），`streamDidRTCPTimeOut` 一次没触发 |
///
/// 为什么报 20 而不是报到顶（实测 4294967295 设备也照收）：报多大，**进程被 SIGKILL 或
/// 崩掉时**那条僵尸会话就占住设备多久——一台设备一次只容一条流，Xcode 的 DeviceHub 也
/// 共用这一格。既然续命现在真的管用，短租期就是白拿的安全垫：正常路径永不到点，异常
/// 路径 20 秒自动腾位置。这也是苹果自己报的数（抓包里它的 `timeout` 就是 20）。
///
/// 这一行同时推翻了过去一整节的结论形态。此前这里的注释写着"实测设备在起流后约 20 秒整
/// 把它结束掉，而且和画面有没有在变、我们回不回 RTCP 都无关"——那句话**观测上全对**，错在
/// 把它读成"设备有一条 20 秒的硬租期"，于是所有力气都花在"找出它认哪一种 RTCP"上：裸 RR、
/// RR+SDES、SR、发到端口+1、按 answer 分配的 SSRC 填发送者、以及 Apple 自己那两种 PT=204
/// 的 AVConference 反馈包（RCTL 20/s + 每帧一个），每一臂都在 20.0 秒整死。那些臂**测的不是
/// "设备认哪种 RTCP"，而是"哪种坏数据报也没到"**——臂与臂之间的差别根本传不到设备。
/// 改那一个整数、死亡时刻就跟着走这一条观测仍然成立（它是计时器本来的斜率）：
///
/// | 请求 `timeout` | answer `RTCPTimeoutInterval` | 结果 |
/// | --- | --- | --- |
/// | 6 | 6 | 最后一个包在 +5.99s |
/// | 20（旧默认） | 20 | +19.99s~+20.05s，前后二十多臂全落在这 |
/// | 30 | 30 | +30.00s |
/// | 3600 | 3600 | 150 秒观察窗跑满（53520 个视频包、150 个 SR 心跳），会话表里还在 |
/// | 4294967295 | 4294967295 | 40 秒窗跑满，会话表里还在 |
///
/// 后面两行（3600 / 报到顶）之所以"活得好好的"和续命无关：计时器在数，只是它数的那个
/// 数比观察窗长得多，而窗跑满时我们一个 RTCP 也没送到过。所以那两行证明的是"租期就是
/// 我们报的那个数"，不是"长租期能代替 RTCP"。两件事现在由同一个观测分开：短租期 + 每秒
/// RR 活得比短租期 + 什么都不发久，而 `pkts in` 正是那个被控住的变量。
constexpr uint32_t kSessionLeaseSeconds = 20;
/// 我们这边回 RTCP 的节奏。1Hz 是照苹果的视频腿实测值来的（它的音频腿也是精确 1.000Hz），
/// 也是上一表里那根"41 个包活过 40 秒"的实测频率。设备只要求"到期前收到过一个"，所以
/// 这一位不需要精准——留出的是 20 倍余量。
constexpr uint64_t kRtcpPeriodMs = 1000;
/// 参考链断掉之后重发 PLI 的间隔。设备实测 20~35ms 就回 IDR，所以这一位纯粹是
/// "别把对端刷屏"的下限；真等不到 IDR 时（>2 秒）由 stall_restart_ms 那条重起接手。
constexpr uint64_t kPliPeriodMs = 1000;
/// 光靠时间戳永远有一段"刚死但还没到阈值"的盲区（实测：静置 20 秒去截图时，会话
/// 其实已经死了 1.3 秒，任何大于 1.3 秒的阈值都会漏）。所以催流那条路在可疑区间
/// 必须去问设备，而不是把阈值调大——调大只会把盲区推到别处。
///
/// 也别把它调回 7.5 秒：那个数来自"最后一个视频包之后 6.9 秒拆流"，而 6.9 秒是拿一个
/// 样本读出来的假象（那次视频包 7.07 秒停、会话在起流后 20.0 秒消失，两个时刻本来没有
/// 因果关系）。退一步说，就算它是对的，拿"视频包静默"当"数据报静默"的阈值也是把两把
/// 不同的尺当成一把：中间隔着设备那每秒一个的 SR，结果是拆完之后有好几秒我们以为流还
/// 活着——用户的手感就是"点了没反应，愣一下画面才跳"。

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

/// 一个 AU 里最大的那个 NAL 有多大。后端装不装得下，只看这一个数。
size_t largest_nal(const std::vector<Nal> &au) {
    size_t m = 0;
    for (const auto &n : au) {
        m = std::max(m, n.size());
    }
    return m;
}

/// 只说一次：这是"配构建时没装 ffmpeg"这一个事实，刷屏对定位没帮助。
void warn_no_software() {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    std::fprintf(stderr,
                 "这台机器上没编软件解码后端（需要 libavcodec），已退回平台后端。"
                 "平台后端只吃 2 字节的 NAL 长度前缀，而这条流单帧能到 256278 字节——"
                 "遇到那种帧会整帧丢弃并重起会话（表现为一次卡顿或花屏）。\n"
                 "装 ffmpeg 的开发头文件后重新配置构建：brew install ffmpeg。\n");
}

}  // namespace

FramePump::FramePump(remote::Device &device, Options options, bool verbose)
    : device_(device), options_(std::move(options)), verbose_(verbose) {}

std::unique_ptr<FramePump> FramePump::start(remote::Device &device, const Options &options,
                                            std::string &err, bool verbose) {
    auto pump = std::unique_ptr<FramePump>(new FramePump(device, options, verbose));
    if (!pump->restart(err)) {
        return nullptr;
    }
    if (!options.record_path.empty()) {
        pump->record_ = std::fopen(options.record_path.c_str(), "wb");
        if (pump->record_ == nullptr) {
            err = "打不开录制文件 " + options.record_path;
            return nullptr;
        }
    }
    // 别在起流的一瞬间就判定"卡住"，那会儿还没有关键帧也正常。
    pump->last_keyframe_ms_ = now_ms();
    pump->last_packet_ms_ = now_ms();
    return pump;
}

FramePump::~FramePump() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    // 收完尾必须把设备侧那条会话停掉。不停的话进程退了、设备还在往一个没人收的端口
    // 编码推流，一直到它自己那 20 秒租期到点——手机白烧电与带宽。更要紧的是同一台设备
    // 同时只容得下一条流：下一条 `startmediastream` 会把这条顶掉（docs §13，
    // `tools/two_session_probe`），所以"上一个进程没停干净"会直接变成"这一个进程每两
    // 秒被拆一次流"。
    //
    // 必须在 join 之后：worker 还在跑的时候它会自己调 restart()，那边也在动 session_。
    if (session_ != nullptr) {
        std::string stop_err;
        if (!session_->stop(device_, stop_err, verbose_)) {
            std::fprintf(stderr, "退出时停不掉设备侧那条流（它会自己活到 20 秒租期结束）: %s\n",
                         stop_err.c_str());
        }
        session_.reset();
    }
    if (record_ != nullptr) {
        std::fclose(record_);
    }
}

bool FramePump::restart(std::string &err) {
    /// 一进来就置位，新会话的第一帧交出来时清掉：取帧方拿它区分"等不到新帧"的两种
    /// 原因（救流还在路上 / 屏幕本来就静止），见头文件里的 reviving()。
    reviving_ = true;

    if (session_ != nullptr) {
        std::string stop_err;
        if (!session_->stop(device_, stop_err, verbose_)) {
            // 停不掉不致命：新会话照样起得来。但旧会话的状态会留在设备上，
            // 排查"第二次连不上"时这是第一个要排除的变量，所以留一行。
            std::fprintf(stderr, "停旧流失败（继续起重流）: %s\n", stop_err.c_str());
        }
        session_.reset();
    }

    StreamSession::Request request;
    request.display_id = options_.display_id;
    request.offer = options_.offer;
    // 显式写出来，不要让"我们要申请多长的租期"和"下面那套接续时刻按多长租期算"分家：
    // 前者是设备侧真正执行的那个数，后者必须与它一致，否则接续要么早得没必要，要么
    // 晚到会话已经没了。
    request.timeout_seconds = kSessionLeaseSeconds;
    session_ = StreamSession::start(device_, request, err, verbose_);
    if (session_ == nullptr) {
        reviving_ = false;  // 没救起来，别让取帧方一直多等
        return false;
    }
    last_packet_ms_ = now_ms();

    if (!worker_running_) {
        worker_running_ = true;
        worker_ = std::thread(&FramePump::loop, this);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.restarts;
    }
    std::printf("媒体会话已重起，收流端口=%u\n", session_->receiver_port());
    return true;
}

void FramePump::loop() {
    std::string err;
    std::vector<uint8_t> datagram;

    /// 已经决定用软解（或者被迫换到软解）。跨会话保持：同一段画面的帧尺寸不会
    /// 突然变小，重起一次就重新试探一次只会多付一次建会话的代价。
    bool software_only = false;
    std::unique_ptr<Decoder> decoder;
    if (!options_.use_hardware) {
        decoder = create_software_decoder();
        software_only = decoder != nullptr;
        if (!software_only) {
            warn_no_software();
        }
    }
    if (decoder == nullptr) {
        decoder = create_platform_decoder();
    }
    bool configured = false;
    std::unique_ptr<scrctl::rt::HevcRtpDepacketizer> depacketizer;

    /// 参考链断了就花 12 字节求一个 IDR。
    ///
    /// 为什么是这里、为什么不是重起会话：实测设备收到 PLI 之后 **20~35ms** 就回一个
    /// IDR（29 次请求换 28~29 个 IDR，一对一），而重起会话要 37~90ms 建会话 + 到第一个
    /// 关键帧才能出图，中间一帧都没有。所以"丢了帧要干净画面"这条路现在是：发 PLI →
    /// 等 IDR → 接着解；`stall_restart_ms` 那条重起逻辑降为**PLI 不管用时的后备**。
    ///
    /// 一句反面教训：**别顺手发 FIR**。参考实现的笔记说"设备不理 PLI、理 FIR"，实测
    /// 正好相反——PLI 有效；FIR 不但换不来 IDR，还会把租期续命整个废掉（设备侧 socket
    /// `pkts in: 40` 说明包到了，但 `Last RTCP packet receive time:nan`，20.13 秒准时死；
    /// 同臂同频同 SSRC，唯一变量就是那个 FIR）。见 docs §13。
    auto request_keyframe = [&] {
        if (now_ms() < next_pli_ms_) {
            return;  // 正在等 IDR，别刷屏
        }
        next_pli_ms_ = now_ms() + kPliPeriodMs;
        last_pli_ms_ = now_ms();
        const auto pli = scrctl::rt::build_pli(session_->started().remote_ssrc,
                                               session_->started().local_ssrc);
        std::string serr;
        if (session_->send_rtp(pli, session_->started().sender_port, serr)) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.pli_sent;
        } else {
            // 发不出去一般是会话已经没了；心跳判据一两秒内会重起，这里不抢它的活。
            std::fprintf(stderr, "PLI 发送失败: %s\n", serr.c_str());
        }
    };

    // 每个会话一套解析器：重起流意味着 AU 边界要从头算，留着半截 NAL 会把新
    // 会话的开头拼进旧会话的尾巴里。
    auto make_parser = [&]() {
        return std::make_unique<AnnexBParser>([&](std::vector<Nal> &&au, bool keyframe) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.aus;
            }
            if (keyframe) {
                std::lock_guard<std::mutex> lock(mutex_);
                last_keyframe_ms_ = now_ms();
            }
            // 后端装不下这个 AU 就换软解。真机实测两种尺寸都会超：主屏 IDR 有
            // 49652~70101 字节，而一次转场动画里的 P 帧切片能到 256278 字节。
            const size_t biggest = largest_nal(au);
            if (biggest > decoder->max_nal_size()) {
                // **换后端的时机只能在关键帧**：换后端等于换一条参考链，非关键帧换
                // 过去对着的是新后端手里没有的参考帧，解出来必花。所以非关键帧这一
                // 帧照旧丢 + 重起会话，只把"下次要用软解"记下来——新会话开头就是
                // IDR，从它起新链刚好。
                software_only = true;
                if (keyframe) {
                    configured = false;
                }
            }
            if (!configured) {
                if (software_only && decoder->max_nal_size() != ~size_t { 0 }) {
                    auto soft = create_software_decoder();
                    if (soft != nullptr) {
                        decoder = std::move(soft);
                        std::printf("解码后端已切换为 %s\n", decoder->backend_name());
                    } else {
                        // 没编软解后端：这条码流平台后端就是解不了。说清楚比默默
                        // 丢帧强——症状是"永远灰屏"，不提示没人会想到去装 ffmpeg。
                        warn_no_software();
                        software_only = false;
                    }
                }
                Nal vps, sps, pps;
                for (const auto &n : au) {
                    if (n.size() < 2) {
                        continue;
                    }
                    switch ((n[0] >> 1) & 0x3F) {
                        case 32: vps = n; break;
                        case 33: sps = n; break;
                        case 34: pps = n; break;
                        default: break;
                    }
                }
                if (vps.empty() || sps.empty() || pps.empty() ||
                    !decoder->configure(vps, sps, pps)) {
                    return;
                }
                configured = true;
            }
            // 换过后端仍然装不下，就是真没能力解这一帧：整帧丢。
            if (biggest > decoder->max_nal_size()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.dropped_oversized;
                    // **丢了这一帧，参考链就断了**：后面每个 P 帧都是拿"缺失的那一帧"
                    // 当参考解的，解出来就是花屏——而这一段会一直持续到我们重起会话为
                    // 止。所以和丢包同一处置：置起 need_keyframe_，在拿到干净关键帧之前
                    // 什么都不解，宁可冻在最后一帧好画上。
                    need_keyframe_ = true;
                }
                // 不要求"已经有过正常画面"：开头 IDR 超大被丢时 decoded 永远是 0，
                // 那个保护恰好把唯一该救的情况排除掉了（用户看到的就是
                // 一片灰且永不恢复）。
                if (oversized_restarts_ >= kMaxOversizedRestarts) {
                    if (!video_unusable_) {
                        video_unusable_ = true;
                        std::printf("连续 %d 次重起都因为单帧超过解码后端上限而拿不到画面："
                                    "这条流在当前后端解不了（画面太复杂）。改成每 %llu 秒才试一次；"
                                    "取帧方请改走截图服务，别再等帧。\n",
                                    oversized_restarts_,
                                    static_cast<unsigned long long>(kOversizedRetryMs / 1000));
                    }
                    // 到顶之后这里不再安排重起——重试由主循环那个退避计时统一管，
                    // 两处都排会互相把对方的间隔吃掉。
                    return;
                }
                // 没到上限时只限频，防死循环。
                if (now_ms() - last_restart_ms_ > 1500) {
                    last_restart_ms_ = now_ms();
                    ++oversized_restarts_;
                    oversized_restart_ = true;
                }
                return;
            }
            // 丢包（序号缺口或分片丢失）之后，参考链已经不可信：非关键帧解了也是
            // 花的，而且会把坏参考继续传下去。所以丢掉一切直到一个**完整**的关键帧。
            // "完整"= 这个关键帧自己的组装期间没再丢包；沾了丢包的关键帧同样不可信。
            const auto &dst = depacketizer->stats();
            const uint64_t loss_now = dst.seq_gaps + dst.dropped_fragments;
            const bool lost_since_prev = loss_now > loss_seen_;
            if (lost_since_prev) {
                need_keyframe_ = true;
            }
            loss_seen_ = loss_now;
            if (need_keyframe_) {
                if (!keyframe || lost_since_prev) {
                    // 正在等干净关键帧：先去要一个，再决定是否丢弃这个 AU。
                    request_keyframe();
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.dropped_awaiting_keyframe;
                    return;
                }
                need_keyframe_ = false;
            }

            // 刻意不清空 publishing_.pixels：clear() 之后 resize() 会把 11MB 重新
            // 写一遍零，等于把省下的分配又换成一次 memset。解码器只在尺寸变了时
            // 才 resize，尺寸没变就是原地覆写。
            Frame &f = publishing_;
            const uint64_t t_decode0 = now_ms();
            const bool ok = decoder->decode(au, f);
            const uint64_t t_published0 = now_ms();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.ms_decode += static_cast<double>(t_published0 - t_decode0);
                ++stats_.decode_calls;
            }
            if (!ok || !f) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.no_output;
                return;
            }
            if (keyframe) {
                ever_keyframe_ = true;
                nokey_restarts_ = 0;  // 只有真解出关键帧才重置上限，防死循环
                // 解出来就说明后端吃得下：退避计数清零，画面再变复杂时还能重新按
                // 正常节奏试，而不是一次性永久判死。
                oversized_restarts_ = 0;
                video_unusable_ = false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            reviving_ = false;  // 救流要交代的"第一帧"到手了，取帧方不必再多等
            // 交换而不是搬走：`frame_ = std::move(f)` 会把 f 的 11MB 缓冲区带走，
            // 下一帧的解码目标就得重新分配并重新缺页——那笔开销实测就是每帧几十毫秒
            // 的主要来源。交换之后 publishing_ 拿回上一帧的缓冲区，尺寸正好，
            // 从第二帧起一次分配都没有。
            std::swap(frame_, publishing_);
            width_ = static_cast<int>(frame_.width);
            height_ = static_cast<int>(frame_.height);
            ++serial_;
            ++stats_.decoded;
            cv_.notify_all();
            stats_.ms_publish += static_cast<double>(now_ms() - t_published0);
        });
    };

    /// 一条会话自己的状态。重起时必须整套换掉——留着旧会话的判断，新会话开头那个
    /// 干净关键帧会被误判成"沾了丢包"而丢掉，画面就冻在旧帧上，要等下一次重起才
    /// 恢复（用户症状：换过后端之后冻住一会儿又自己好了）。
    auto new_session_state = [&] {
        depacketizer = std::make_unique<scrctl::rt::HevcRtpDepacketizer>(session_->started().payload_type);
        configured = false;
        parser_ = make_parser();
        session_start_ms_ = now_ms();
        last_packet_ms_ = session_start_ms_;
        // 新会话一起就马上补一个续命包：设备那个计时器是从"上次收到我们 RTCP"开始倒数的，
        // 第一秒就送到，租期才完整可用（等第一个心跳周期再发等于白送掉一秒）。
        next_rtcp_ms_ = session_start_ms_;
        // 新会话开头自带一个干净 IDR，所以这里不是"等着要关键帧"的状态：清零让第一次
        // 真的断链时能立刻发出 PLI，而不是等上一轮的节流。
        next_pli_ms_ = 0;
        last_pli_ms_ = 0;
        ever_keyframe_ = false;
        need_keyframe_ = false;
        loss_seen_ = 0;
        gaps_at_last_check_ = 0;
        // 设备的 SR 累计数每条会话从零重数，我们的 packets 跨会话连着涨。留下这个
        // 基线，读数才是在同一条数轴上比（见 Stats::session_packets_base）。
        // 设备那一侧则反过来：它的数要跟着会话归零，否则重起后的第一秒里读数是
        // "设备 11872 我 83"，像是把一万包凭空丢了，而真相只是这一会话的第一条 SR
        // 还没到。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.session_packets_base = stats_.packets;
            stats_.dev_sent_packets = 0;
            stats_.dev_sent_octets = 0;
        }
    };
    new_session_state();

    /// 流已经不来了（设备在画面静止时会自己把流结束掉）时，问一句"我们这条还在
    /// 设备上吗"，不在就重起。`why` 只用于日志：是静默到点催的，还是用户操作催的。
    /// `quiet_ms` 是判据用的那段静默时长，打出来是为了能事后对账：救流到底花了几秒，
    /// 只有这个数说得清（阈值调一档，日志里就应该看得见它动了）。
    auto revive_if_dead = [&](const char *why, uint64_t quiet_ms) {
        // 置位在问设备**之前**：那一条 RPC 自己就要 100~300ms，取帧方在这段时间里
        // 读到 false 就会把旧帧交出去。
        reviving_ = true;
        std::string perr;
        const auto state = StreamSession::probe(device_, session_->started().session_uuid, perr,
                                                verbose_);
        if (state == StreamSession::ServerState::Alive) {
            reviving_ = false;
            if (verbose_) {
                std::printf("%s：静默 %llums，但设备说这条流还活着（画面本来就静止），什么都不做\n",
                            why, static_cast<unsigned long long>(quiet_ms));
            }
            return false;  // 流活着，只是画面没变化——什么都不做才是对的
        }
        std::printf("%s：静默 %llums，%s，重起媒体会话\n", why,
                    static_cast<unsigned long long>(quiet_ms),
                    state == StreamSession::ServerState::Ended
                        ? "设备已结束这条流"
                        : ("问不到流状态（" + perr + "）").c_str());
        std::string restart_err;
        if (!restart(restart_err)) {
            std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
            reviving_ = false;
            return false;
        }
        new_session_state();
        return true;
    };

    /// 不问状态，直接重起。用户动手时走这条：问一次会话状态是一条 RPC（实测
    /// 100~300ms），而停掉再加起回来只要 37~90ms（tools/restart_gap_probe 量的）。
    /// 先问再起重等于把手感里最大的一笔开销花在"确认一个本来就打算处理的事实"上。
    /// 会话其实还活着时重起也不亏：新会话必然带一个干净的关键帧，画面立刻是最新的。
    auto restart_now = [&]() {
        std::string restart_err;
        if (!restart(restart_err)) {
            std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
            return false;
        }
        new_session_state();
        return true;
    };

    /// 流静默之后该不该救、怎么救。三档，判据是"距离最后一个数据报多久"，而设备的
    /// RTCP SR 每秒一个就是这条流的心跳（画面完全静止也照发，所以心跳停 = 会话没了）：
    ///   静默 ≤ 1.2s   心跳还在，流活着。什么都不做（画面在动时这是常见情况）
    ///   1.2s ~ blind_at  可疑：流死了，也可能只是连着丢了 SR。问设备一句
    ///                    （getmediastreamserverstatus，实测 100~300ms），死了才重起
    ///   > blind_at       不再为一次 RPC 花时间，直接重起（停掉再加起 37~90ms）
    ///
    /// 催流（wake()）和静默自催共用它：同一件事不该因为发现的人是"用户的手"还是
    /// "定时器"就有不同的容忍度。以前静默自催是单独一套——盲等 silence_restart_ms
    /// （3 秒）才问一句，于是画面从设备最后一包到我们重起好要冻 3 秒多。现在这三档
    /// 只是**兜底**：正常节拍由"租期将到主动接续"那条分支负责。以前那条分支每 20 秒
    /// 就走一次（当时以为设备有一条 20 秒硬租期，喂画面、回 RTCP、查状态都续不上，
    /// docs §13 有那张对照表），后来发现那 20 秒就是我们自己在请求里报的数，报成一小时
    /// 之后它一小时才走一次——所以这三档判活的兜底反而变回了主要手段：流不是因为租期
    /// 停的，是因为设备侧真的停了（见 kSessionLeaseSeconds 上面那张表）。
    ///
    /// `blind_at`：催流用两个心跳（2.5s，用户正在等，不值得再花一条 RPC 去确认一个
    /// 本来就打算处理的事实），静默自催用 silence_restart_ms。
    /// `ask_every_ms` 只给中间那一档节流：静默时 last_packet_ms_ 不动，quiet 会一直
    /// 停在阈值之上，而定时轮子是 50ms 一圈——不设门槛就是每 50ms 一条 RPC 的风暴。
    /// 催流那条路传 0（不节流）：那是有人正在等，且 wake_requested_ 是一个 bool，
    /// RPC 期间攒下的催不会变成并发的一串。
    uint64_t last_ask_ms_ = 0;
    auto judge_quiet = [&](uint64_t quiet, uint64_t blind_at, uint64_t ask_every_ms,
                           const char *why) {
        if (quiet > blind_at) {
            std::printf("%s：最后一个数据报已静默 %llums，连着两个每秒 SR 都没来，重起媒体会话\n",
                        why, static_cast<unsigned long long>(quiet));
            restart_now();
        } else if (quiet > kQuietSuspiciousMs) {
            const uint64_t now = now_ms();
            if (ask_every_ms == 0 || now - last_ask_ms_ >= ask_every_ms) {
                last_ask_ms_ = now;
                revive_if_dead(why, quiet);
            }
        }
    };

    for (;;) {
        // **续命包**：每秒一个 RR。这一位不是可选的礼貌——设备那个 `RTCPTimeoutInterval`
        // 是"距离上次收到我们 RTCP 多久"的空闲计时器（推导与实测表见 kSessionLeaseSeconds
        // 上面），租期报 20 秒而不回 RTCP，会话就必然在 +19.97s 被设备摘掉。
        //
        // 放在循环最顶端而不是"读包超时"那条分支里：快速动画面下包是连着的，50ms 超时
        // 永远轮不到，挂在超时上的话保活包在这种时候一个都发不出去。
        //
        // 形状是探针里唯一实测有效的那一种（`--what rrsrc --hz 1`：设备侧 socket
        // `pkts in: 41`、活过 40 秒）：裸 RR 32 字节，发送者 SSRC 填 answer 给我们分配的
        // `RemoteSSRC`，报告块指认设备的 `LocalSSRC`，目的端口是 `sender.port`
        // （= `streamConfig.SourcePort`，21/21 次实测这两个数相等）。
        if (now_ms() >= next_rtcp_ms_) {
            next_rtcp_ms_ += kRtcpPeriodMs;
            const auto rr = scrctl::rt::build_rr(session_->started().remote_ssrc,
                                                 session_->started().local_ssrc,
                                                 depacketizer ? depacketizer->last_sequence() : 0);
            std::string serr;
            const bool ok = session_->send_rtp(rr, session_->started().sender_port, serr);
            uint64_t failed_after = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) {
                    ++stats_.rtcp_sent;
                } else {
                    failed_after = ++stats_.rtcp_failed;
                }
            }
            if (failed_after == 1) {
                // 只报第一条：发不出去一般是会话已经没了，而下面那套心跳判据一秒钟内就
                // 会把它重起掉。每 50ms 刷一条反而盖住真正该看的那行日志。
                std::fprintf(stderr, "RTCP 保活包发送失败: %s\n", serr.c_str());
            }
        }
        // 用户动了手（或者自动化框架来取帧了）：按 judge_quiet 那三档心跳判据处理。
        // 中间那一档不能省——光靠时间戳永远有"刚死但还没到阈值"的盲区（实测静置 20 秒
        // 去截图时会话已经死了 1.3 秒），而把阈值调大只会把盲区推到别处。
        if (wake_requested_.exchange(false)) {
            judge_quiet(now_ms() - last_packet_ms_, kQuietCertainMs, 0, "收到操作");
            continue;
        }
        // "该重起了"这个判断必须每轮都做，不能只挂在"读包超时"那条分支上。快速动
        // 画面下包是连续到达的，50ms 超时永远轮不到，于是"丢了帧要去拿新关键帧"这个
        // 决定会一直悬着——实测连丢 20 帧、画面冻住十几秒才等到一次超时才恢复。
        // 退避计时必须由泵自己走，不能挂在"又收到一个超大 AU"上：降级之后会话早就被
        // 设备拆掉了，一个包都收不到，那个条件永远不成立，降级就成了一个出不来的坑
        // （调用方会永远停在截图服务上，哪怕画面早就变简单了）。
        if (video_unusable_ && now_ms() - last_restart_ms_ >= kOversizedRetryMs) {
            last_restart_ms_ = now_ms();
            oversized_restart_ = true;
            std::printf("降级满 %llu 秒，再试一次这条视频流（画面可能已经变简单了）\n",
                        static_cast<unsigned long long>(kOversizedRetryMs / 1000));
        }
        if (oversized_restart_) {
            oversized_restart_ = false;
            std::printf("有 NAL 超过平台后端的长度前缀上限，重起媒体会话拿新关键帧\n");
            std::string restart_err;
            if (!restart(restart_err)) {
                std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                new_session_state();
            }
            continue;
        }
        if (!session_->next_packet(datagram, 50, err)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            // 一个包都不来了：设备已经把我们这条流结束掉了（实测它会在几分钟之后
            // 自己停，且我们从不回 RTCP 接收报告）。不重起的话用户看到的就是
            // "窗口冻住"，而进程、线程、隧道全都好着——最难往流上想。
            if (!ever_keyframe_ && now_ms() - session_start_ms_ > 5000 &&
                nokey_restarts_ < 3) {
                ++nokey_restarts_;
                session_start_ms_ = now_ms();
                std::printf("起流 %d 秒仍未解出关键帧（开头 IDR 可能被丢），重起媒体会话 (%d/3)\n",
                            5, nokey_restarts_);
                std::string restart_err;
                if (!restart(restart_err)) {
                    std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                new_session_state();
                continue;
            }
            if (options_.silence_restart_ms > 0) {
                const uint64_t quiet = now_ms() - last_packet_ms_;
                judge_quiet(quiet, std::max<uint64_t>(kQuietCertainMs,
                                                       static_cast<uint64_t>(options_.silence_restart_ms)),
                            kSrPeriodMs, "静默超时");
            }
            continue;  // 超时不是结束
        }
        last_packet_ms_ = now_ms();
        // 设备的 SR 是裸 RTCP（开头 0x81 0xc8），混在视频同一个端口上每秒来一个。
        // 它自带的"累计已发视频包数"在偏移 20，是设备侧的权威计数。
        const bool is_sr = scrctl::rt::is_rtcp_sr(datagram);
        uint64_t dev_pkts = 0, dev_octets = 0;
        if (is_sr) {
            const auto be32 = [&datagram](std::size_t off) {
                return (uint64_t(datagram[off]) << 24) | (uint64_t(datagram[off + 1]) << 16) |
                       (uint64_t(datagram[off + 2]) << 8) | datagram[off + 3];
            };
            dev_pkts = be32(20);
            dev_octets = be32(24);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            ++stats_.packets;
            if (is_sr) {
                ++stats_.sr_packets;
            } else {
                ++stats_.video_packets;
            }
            if (dev_pkts != 0) {
                stats_.dev_sent_packets = dev_pkts;
                stats_.dev_sent_octets = dev_octets;
            }
        }

        std::vector<uint8_t> bytes;
        const uint64_t t_dp0 = now_ms();
        const bool pushed = depacketizer->push(datagram, bytes, err);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.other_payload = depacketizer->stats().other_payload;
            stats_.ms_depacketize += static_cast<double>(now_ms() - t_dp0);
        }
        if (!pushed || bytes.empty()) {
            continue;
        }
        if (record_ != nullptr) {
            std::fwrite(bytes.data(), 1, bytes.size(), record_);
        }
        parser_->feed(bytes.data(), bytes.size());

        // 卡住的判据要两条同时成立：只看序号缺口会误伤（丢一个分片也许下一帧
        // 就是关键帧），只看"多久没关键帧"又会在静止画面上白白重起。
        if (options_.stall_restart_ms > 0) {
            const auto &dst = depacketizer->stats();
            const uint64_t gaps = dst.seq_gaps;
            bool stalled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // 现在这条判据要**三条**同时成立，第三条是新加的：
                // 1) 出现了新的序号缺口；
                // 2) 距上一个关键帧超过 stall_restart_ms；
                // 3) **我们已经发过 PLI、而且等了它这么久**。
                //
                // 第 3 条为什么必需：这条流的 IDR 有多稀疏？实测基线 30 秒只有 1 个
                // （起流那一下），所以第 2 条几乎是恒真的——任何一个缺口都会立刻重起。
                // 修好 UDP 之后 PLI 20~35ms 就能换来 IDR，重起（~300ms 且中间无帧）
                // 应该退到"PLI 不管用时"才发生。不加第 3 条，PLI 那条路根本走不到：
                // 临时注入一个真实丢包的实验里，缺口出现同一次循环里就重起了，
                // IDR 虽然在 35ms 后到、却已经用不上。
                stalled = gaps > gaps_at_last_check_ && last_pli_ms_ != 0 &&
                          now_ms() - last_pli_ms_ >=
                              static_cast<uint64_t>(options_.stall_restart_ms) &&
                          now_ms() - last_keyframe_ms_ >
                              static_cast<uint64_t>(options_.stall_restart_ms);
                gaps_at_last_check_ = gaps;
                stats_.gaps = gaps;
                stats_.dropped_fragments = dst.dropped_fragments;
                if (stalled) {
                    last_keyframe_ms_ = now_ms();  // 给新会话留出时间，别连着撞
                }
            }
            if (stalled) {
                std::string restart_err;
                if (!restart(restart_err)) {
                    std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                new_session_state();
            }
        }
    }
}

bool FramePump::latest(Frame &out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [&] { return serial_ > 0 || stopping_; });
    if (serial_ == 0) {
        return false;
    }
    out = frame_;
    return true;
}

uint64_t FramePump::newer(Frame &out, uint64_t since, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [&] { return serial_ > since || stopping_; });
    if (serial_ <= since) {
        return 0;
    }
    out = frame_;
    return serial_;
}

uint64_t FramePump::serial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

FramePump::Stats FramePump::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

uint8_t FramePump::payload_type() const {
    return session_ != nullptr ? session_->started().payload_type : 0;
}

uint16_t FramePump::receiver_port() const {
    return session_ != nullptr ? session_->receiver_port() : 0;
}

void FramePump::size(int &width, int &height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    width = width_;
    height = height_;
}

DisplayCrop display_crop(int coded_w, int coded_h) {
    if (coded_w == 1136 && coded_h == 2464) {
        return DisplayCrop {0, 0, 1125, 2436};
    }
    return DisplayCrop {0, 0, coded_w, coded_h};
}

}  // namespace scrctl::media
