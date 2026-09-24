#include "media/FramePump.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "bitstream/AnnexB.h"
#include "media/StreamSession.h"
#include "remote/Device.h"

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
/// 光靠时间戳永远有一段"刚死但还没到阈值"的盲区（实测：静置 20 秒去截图时，会话
/// 其实已经死了 1.3 秒，任何大于 1.3 秒的阈值都会漏）。所以催流那条路在可疑区间
/// 必须去问设备，而不是把阈值调大——调大只会把盲区推到别处。
///
/// 也别把它调回 7.5 秒：那是"最后一个视频包之后 6.9 秒"这个实测拆流时长，量的是
/// 视频包而不是数据报，拿它当数据报静默的阈值等于把两把不同的尺当成一把，结果是
/// 拆完流之后有 5 秒多的窗口里我们以为流还活着——用户的手感就是"点了没反应，愣
/// 一下画面才跳"。

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
                // 一片灰且永不恢复）。只限频，防死循环。
                if (now_ms() - last_restart_ms_ > 1500) {
                    last_restart_ms_ = now_ms();
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
        ever_keyframe_ = false;
        need_keyframe_ = false;
        loss_seen_ = 0;
        gaps_at_last_check_ = 0;
    };
    new_session_state();

    /// 流已经不来了（设备在画面静止时会自己把流结束掉）时，问一句"我们这条还在
    /// 设备上吗"，不在就重起。`why` 只用于日志：是静默到点催的，还是用户操作催的。
    auto revive_if_dead = [&](const char *why) {
        // 置位在问设备**之前**：那一条 RPC 自己就要 100~300ms，取帧方在这段时间里
        // 读到 false 就会把旧帧交出去。
        reviving_ = true;
        std::string perr;
        const auto state = StreamSession::probe(device_, session_->started().session_uuid, perr,
                                                verbose_);
        if (state == StreamSession::ServerState::Alive) {
            reviving_ = false;
            return false;  // 流活着，只是画面没变化——什么都不做才是对的
        }
        std::printf("%s：%s，重起媒体会话\n",
                    why,
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

    for (;;) {
        // 用户动了手（或者自动化框架来取帧了）。分三档处理，判据是"距离最后一个数据报
        // 多久"，而设备的 RTCP SR 每秒一个就是这条流的心跳：
        //   静默 ≤ 1.2s   心跳还在，流活着。什么都不做（这是画面在动时的常见情况）
        //   1.2s ~ 2.5s   可疑：可能是流死了，也可能是连着丢了 SR。问设备一句
        //                 （getmediastreamserverstatus，实测 100~300ms），死了才重起
        //   > 2.5s        两个心跳都没了，不再为一次 RPC 花时间，直接重起（37~90ms）
        // 中间那一档不能省：光靠时间戳永远有"刚死但还没到阈值"的盲区（实测静置 20 秒
        // 去截图时会话已经死了 1.3 秒），而把阈值调大只会把盲区推到别处。
        if (wake_requested_.exchange(false)) {
            const uint64_t quiet = now_ms() - last_packet_ms_;
            if (quiet > kQuietCertainMs) {
                std::printf("收到操作（最后一个数据报已静默 %llums，连着两个每秒 SR 都没来），"
                            "重起媒体会话\n",
                            static_cast<unsigned long long>(quiet));
                restart_now();
            } else if (quiet > kQuietSuspiciousMs) {
                revive_if_dead("收到操作");
            }
            continue;
        }
        // "该重起了"这个判断必须每轮都做，不能只挂在"读包超时"那条分支上。快速动
        // 画面下包是连续到达的，50ms 超时永远轮不到，于是"丢了帧要去拿新关键帧"这个
        // 决定会一直悬着——实测连丢 20 帧、画面冻住十几秒才等到一次超时才恢复。
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
            if (options_.silence_restart_ms > 0 &&
                now_ms() - last_packet_ms_ > static_cast<uint64_t>(options_.silence_restart_ms)) {
                last_packet_ms_ = now_ms();
                revive_if_dead("静默超时");
            }
            continue;  // 超时不是结束
        }
        last_packet_ms_ = now_ms();
        // 设备的 SR 是裸 RTCP（开头 0x81 0xc8），混在视频同一个端口上每秒来一个。
        // 它自带的"累计已发视频包数"在偏移 20，是设备侧的权威计数。
        uint64_t dev_pkts = 0, dev_octets = 0;
        if (datagram.size() >= 28 && datagram[0] == 0x81 && datagram[1] == 0xc8) {
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
                stalled = gaps > gaps_at_last_check_ &&
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
