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

/// 设备 SR 的典型周期为一秒，画面静止时也曾观察到持续发送。
/// 作为静默阈值的基准，但缺少 SR 本身不证明会话已经终止。
constexpr uint64_t kSrPeriodMs = 1000;
/// 超过一个 SR 周期加余量后查询设备状态，区分会话结束与 UDP 丢失。
constexpr uint64_t kQuietSuspiciousMs = kSrPeriodMs + 200;
/// 超过两个 SR 周期加余量后直接重建，减少继续查询造成的恢复延迟。
constexpr uint64_t kQuietCertainMs = 2 * kSrPeriodMs + 500;
/// 连续三次因帧过大重建后降级，避免对后端持续不支持的画面频繁建立会话。
/// 实测复杂画面的 IDR 可达 256278 字节，重建并不保证帧尺寸变小。
constexpr int kMaxOversizedRestarts = 3;
/// 降级期间每分钟重试，允许画面复杂度降低后恢复；成功解码后回到正常策略。
constexpr uint64_t kOversizedRetryMs = 60000;
/// 启动五秒后仍无关键帧输出则重建，处理首个 IDR 丢失或解码无输出。
constexpr uint64_t kNokeyBlindMs = 5000;
/// 无关键帧的快速重试上限。达到后转为降级及定时重试，不能永久停止；
/// SR 持续到达时，静默恢复无法处理这种情况。
constexpr int kMaxNokeyRestarts = 3;
/// startmediastream 请求的 timeout 决定 answer 的 RTCPTimeoutInterval。
/// 设备从上次收到有效 RTCP 开始计时；周期 RR 可延长会话，不是启动后的硬到期。
/// 采用 20 秒，兼顾异常退出后设备会话占用的释放时间。修复 UDP 封包后，
/// 真机在每秒 RR 下运行超过 40 秒；无反馈约 20 秒结束。实验表见
/// docs/coredevice.md §13，较大的 timeout 仅延长无反馈时的观察窗口。
constexpr uint32_t kSessionLeaseSeconds = 20;
/// 每秒发送 RR，沿用已验证的视频反馈频率，较 20 秒租期留有余量。
constexpr uint64_t kRtcpPeriodMs = 1000;
/// 等待关键帧期间每秒重发 PLI，限制请求频率。真机曾在 20–35 ms 内
/// 响应 IDR；未恢复时由 stall_restart_ms 的后备会话重建处理。
constexpr uint64_t kPliPeriodMs = 1000;
/// 静默判断使用最后一个数据报而非最后一个视频包，画面不变时 SR 仍可能
/// 到达。可疑区间查询设备，避免仅凭旧视频帧误判连接。

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

/// 返回 AU 内最大 NAL 的长度，用于判断后端长度上限。
size_t largest_nal(const std::vector<Nal> &au) {
    size_t m = 0;
    for (const auto &n : au) {
        m = std::max(m, n.size());
    }
    return m;
}

/// 软件解码不可用的提示只输出一次，避免每次重建重复提示同一构建条件。
void warn_no_software() {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    std::fprintf(stderr,
                 "构建未包含软件解码后端（libavcodec），继续使用平台后端。"
                 "当前平台适配使用 2 字节 NAL 长度前缀，无法处理超过其长度上限的 NAL。"
                 "遇到此类帧会丢弃 AU 并尝试重建会话，可能暂时无新画面。\n"
                 "请安装 FFmpeg 开发包并重新配置、构建项目。\n");
}

}  // namespace

NokeyAction plan_nokey(const int restarts, const int max_restarts, const bool already_unusable) {
    if (restarts < max_restarts) {
        return NokeyAction::kRetry;
    }
    // 达到上限后首次标记降级，已降级则等待统一退避计时。
    return already_unusable ? NokeyAction::kWait : NokeyAction::kDegrade;
}

FramePump::FramePump(remote::Device &device, Options options, bool verbose)
    : device_(device), options_(std::move(options)), verbose_(verbose) {}

std::unique_ptr<FramePump> FramePump::start(remote::Device &device, const Options &options,
                                            std::string &err, bool verbose) {
    auto pump = std::unique_ptr<FramePump>(new FramePump(device, options, verbose));
    // 缺少解码后端时不建立设备媒体会话，避免占用设备资源。
    // 使用编译期能力常量，无需创建平台解码会话试探。
    if (!scrctl::kHaveDecoder) {
        err = scrctl::kNoDecoderMessage;
        return nullptr;
    }
    // restart() 仅建立会话；record_ 和 last_keyframe_ms_ 等字段完成初始化
    // 后才创建 worker，避免线程读到尚未初始化的无锁状态。
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
    // 首次关键帧尚未到达时不立即判定停顿；last_packet_ms_ 由 restart() 设置。
    pump->last_keyframe_ms_ = now_ms();
    pump->worker_running_ = true;
    pump->worker_ = std::thread(&FramePump::loop, pump.get());
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
    // 先 join worker，再停止设备媒体会话；worker 可能调用 restart() 并修改
    // session_。正常退出显式停流，避免设备继续向无人接收的端口编码，
    // 也避免会话占用影响下一次启动。异常退出仍依赖租期释放。
    if (session_ != nullptr) {
        std::string stop_err;
        if (!session_->stop(device_, stop_err, verbose_)) {
            std::fprintf(stderr, "停止设备视频会话失败（设备端将在租期到期后释放）: %s\n",
                         stop_err.c_str());
        }
        session_.reset();
    }
    if (record_ != nullptr) {
        std::fclose(record_);
    }
}

bool FramePump::restart(std::string &err) {
    /// 开始重建时标记恢复中，新会话首帧输出后清除，供调用方区分恢复和静止。
    reviving_ = true;

    if (session_ != nullptr) {
        std::string stop_err;
        if (!session_->stop(device_, stop_err, verbose_)) {
            // 停止旧会话失败仍尝试新会话，保留错误供排查设备端残留状态。
            std::fprintf(stderr, "停止旧视频会话失败（仍尝试重建）: %s\n", stop_err.c_str());
        }
        session_.reset();
    }

    StreamSession::Request request;
    request.display_id = options_.display_id;
    request.offer = options_.offer;
    // 请求租期使用统一常量，保证保活策略与设备执行的租期一致。
    request.timeout_seconds = kSessionLeaseSeconds;
    session_ = StreamSession::start(device_, request, err, verbose_);
    if (session_ == nullptr) {
        reviving_ = false;  // 重建失败，结束本次恢复标记。
        return false;
    }
    last_packet_ms_ = now_ms();

    // 首次启动时 worker 尚未运行，不计入重建次数，也不输出重建提示。
    // 线程创建由 start() 统一管理。
    if (!worker_running_) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.restarts;
    }
    std::printf("视频会话已重建，收流端口=%u\n", session_->receiver_port());
    return true;
}

void FramePump::loop() {
    std::string err;
    std::vector<uint8_t> datagram;

    /// 软件解码选择跨会话保留，避免每次重建重复尝试能力不足的硬件后端。
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
    // 两种后端都不可用时退出，避免解引用空 decoder。start() 通常已通过
    // 编译期能力检查拦截，此处仍保留防御检查。
    if (decoder == nullptr) {
        std::fprintf(stderr, "%s", scrctl::kNoDecoderMessage);
        return;
    }
    bool configured = false;
    std::unique_ptr<scrctl::rt::HevcRtpDepacketizer> depacketizer;
    /// 探针用视频接收计数，不包含 SR。
    uint64_t video_seen = 0;

    /// 丢包后优先用 12 字节 PLI 请求 IDR，再等待完整关键帧解码；重建作为后备。
    /// 真机 PLI 响应曾为 20–35 ms，重建会话 RPC 约 37–90 ms，另需等待首帧。
    /// 不发送 FIR：当前设备实测 FIR 未生成 IDR，且影响 RTCP 租期保活，
    /// 即使设备 socket 计数确认收到。详细对照见 docs/coredevice.md §13。
    auto request_keyframe = [&] {
        // 在所有提前返回之前记录本轮等待起点，包括探针禁止 PLI 的情况。
        // 后备超时从首次请求计算，不能使用每秒重发都会更新的 last_pli_ms_。
        if (first_pli_ms_ == 0) {
            first_pli_ms_ = now_ms();
        }
        if (options_.debug_suppress_pli || pli_off_) {
            // 测试模式可禁止 PLI，用来单独验证等待超时后的会话重建路径。
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.pli_suppressed;
            return;
        }
        if (now_ms() < next_pli_ms_) {
            return;  // 限制重复关键帧请求的频率。
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
            // 发送失败保留诊断；后续静默与关键帧等待策略负责恢复。
            std::fprintf(stderr, "PLI 发送失败: %s\n", serr.c_str());
        }
    };

    /// 每次拆包后立即检查序号缺口和作废分片，启动关键帧等待与后备计时。
    /// 不能只在 AU 回调检查：丢包后可能只剩 SR，不再产生 AU。
    auto note_loss = [&] {
        if (depacketizer == nullptr) {
            return;
        }
        const auto &dst = depacketizer->stats();
        const uint64_t loss_now = dst.seq_gaps + dst.dropped_fragments;
        if (loss_now <= loss_seen_) {
            return;
        }
        loss_seen_ = loss_now;
        loss_since_au_ = true;
        awaiting_idr_from_loss_ = true;
        if (!need_keyframe_) {
            // 仅在进入关键帧等待状态时输出一次。
            std::printf("检测到序号缺口或丢弃分片：请求关键帧，等待上限 %d ms\n",
                        options_.stall_restart_ms);
        }
        need_keyframe_ = true;
        request_keyframe();
    };

    // 每个会话重新创建解析器，避免将旧会话未完成的 NAL 或 AU 拼入新会话。
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
            // NAL 超过平台后端的长度上限时尝试软件解码。实测 IDR 可达
            // 49652–70101 字节，转场中的非关键帧切片也曾达到 256278 字节。
            const size_t biggest = largest_nal(au);
            if (biggest > decoder->max_nal_size()) {
                // 仅在关键帧切换后端，新解码器尚无旧参考帧。超大非关键帧只记录
                // 后续使用软件解码，并通过重建会话取得用于初始化的 IDR。
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
                        // 软件后端未编入时保留平台后端并提示构建条件。
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
            // 当前后端仍不支持此 NAL 尺寸时丢弃整个 AU。
            if (biggest > decoder->max_nal_size()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.dropped_oversized;
                    // 丢弃 AU 可能破坏参考链，暂停解码后续非关键帧，保留最近的有效画面。
                    need_keyframe_ = true;
                }
                // 首次 IDR 也可能超过上限，即使尚无成功解码记录仍需启动恢复。
                if (oversized_restarts_ >= kMaxOversizedRestarts) {
                    if (!video_unusable_) {
                        video_unusable_ = true;
                        std::printf("连续 %d 次重建仍因 NAL 超过后端长度上限而无画面输出。"
                                    "视频暂不可用，将每 %llu 秒重试；"
                                    "调用方可切换至截图服务。\n",
                                    oversized_restarts_,
                                    static_cast<unsigned long long>(kOversizedRetryMs / 1000));
                    }
                    // 达到重建上限后交由主循环统一管理降级重试，避免重复调度。
                    return;
                }
                // 尚未达到上限时限制重建频率。
                if (now_ms() - last_restart_ms_ > 1500) {
                    last_restart_ms_ = now_ms();
                    ++oversized_restarts_;
                    oversized_restart_ = true;
                }
                return;
            }
            // 丢包后等待完整关键帧，避免继续使用可能受损的参考链。
            // note_loss() 在收包时启动等待；此处消费自上个 AU 以来的丢包标记，
            // 用于判断本次关键帧的组装过程是否也受到丢包影响。
            const bool lost_since_prev = loss_since_au_;
            loss_since_au_ = false;
            // 保存进入回调时的等待状态，成功解码并取得像素后才解除。
            const bool was_awaiting = need_keyframe_;
            // 故障注入使用进入回调时的快照，避免测试触发条件依赖随后被修改的
            // 恢复状态。否则提前清除状态会使注入失效，无法验证恢复逻辑。
            const bool awaiting_at_entry = awaiting_idr_from_loss_;
            if (need_keyframe_) {
                if (!keyframe || lost_since_prev) {
                    // 不完整关键帧及非关键帧不能解除等待，继续请求关键帧。
                    request_keyframe();
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.dropped_awaiting_keyframe;
                    return;
                }
            }

            // 保留 publishing_.pixels 的容量和尺寸。同尺寸时由解码器直接覆写，
            // 避免 clear() 后 resize() 对整个图像缓冲区重新填零。
            Frame &f = publishing_;
            const uint64_t t_decode0 = now_ms();
            bool ok = false;
            // 默认仅向丢包恢复期间的关键帧注入失败，验证解码无输出后仍会继续
            // 等待并触发恢复。debug_fail_any_keyframe 可扩大到启动阶段。
            if (keyframe && (awaiting_at_entry || options_.debug_fail_any_keyframe) &&
                options_.debug_fail_decode_of_keyframe > 0) {
                --options_.debug_fail_decode_of_keyframe;
                // 可同时禁止后续 PLI，排除新关键帧请求对会话重建测试的影响。
                if (options_.debug_suppress_pli_after_fail) {
                    pli_off_ = true;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.forced_decode_failures;
                }
                std::printf("测试：注入关键帧解码失败（"
                            "%s）\n",
                            options_.debug_suppress_pli_after_fail ? "后续禁止 PLI"
                                                                   : "继续发送 PLI");
            } else {
                ok = decoder->decode(au, f);
            }
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
            // 仅在解码成功且输出有效像素后解除等待。解析出完整关键帧并不保证
            // 解码器输出图像，过早清除会丢失 first_pli_ms_ 对应的后备恢复计时。
            if (was_awaiting) {
                need_keyframe_ = false;
                awaiting_idr_from_loss_ = false;
                first_pli_ms_ = 0;
            }
            if (keyframe) {
                ever_keyframe_ = true;
                nokey_restarts_ = 0;  // 成功输出关键帧后重置快速重试计数。
                // 成功解码后退出降级并重置尺寸限制重试计数。
                oversized_restarts_ = 0;
                video_unusable_ = false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            reviving_ = false;  // 新会话已输出第一帧，结束恢复等待。
            // 交换两个帧缓冲区，下一次解码复用上一帧的内存，避免 move 后
            // 解码目标失去缓冲区并重新分配。尺寸变化仍由解码器处理。
            std::swap(frame_, publishing_);
            width_ = static_cast<int>(frame_.width);
            height_ = static_cast<int>(frame_.height);
            ++serial_;
            ++stats_.decoded;
            cv_.notify_all();
            stats_.ms_publish += static_cast<double>(now_ms() - t_published0);
        });
    };

    /// 重建后重置解析器、计时器及丢包标记，避免旧状态导致新会话首个
    /// 关键帧被误判为不完整。跨会话的重试策略与测试选项另行保留。
    auto new_session_state = [&] {
        depacketizer = std::make_unique<scrctl::rt::HevcRtpDepacketizer>(session_->started().payload_type);
        configured = false;
        parser_ = make_parser();
        session_start_ms_ = now_ms();
        last_packet_ms_ = session_start_ms_;
        // 新会话立即发送第一次 RR，此后按周期续期。
        next_rtcp_ms_ = session_start_ms_;
        // 重置关键帧请求节流与等待状态，使后续首次丢包可以立即请求 PLI。
        next_pli_ms_ = 0;
        last_pli_ms_ = 0;
        first_pli_ms_ = 0;
        awaiting_idr_from_loss_ = false;
        ever_keyframe_ = false;
        need_keyframe_ = false;
        loss_seen_ = 0;
        loss_since_au_ = false;
        // 测试禁止 PLI 的选项跨会话保留，继续隔离会话重建恢复路径。
        // 视频故障注入的计数则按会话归零，避免新会话也被持续忽略。
        video_seen = 0;
        // SR 中的设备视频包计数按会话归零，本地数据报总数跨会话累计。
        // 保存本地会话起点并清空旧 SR 读数，避免新 SR 到达前显示旧设备计数。
        // 两者统计对象不同：本地含 RTCP，设备只报视频，不能直接相减估计丢包。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.session_packets_base = stats_.packets;
            stats_.dev_sent_packets = 0;
            stats_.dev_sent_octets = 0;
        }
    };
    new_session_state();

    /// 可疑静默时查询设备会话状态；已结束或查询失败则尝试重建。
    /// why 和 quiet_ms 仅用于说明触发来源与已等待时间。
    auto revive_if_dead = [&](const char *why, uint64_t quiet_ms) {
        // 查询前标记恢复中，避免调用方在耗时 RPC 期间将旧帧当作已恢复输出。
        reviving_ = true;
        std::string perr;
        const auto state = StreamSession::probe(device_, session_->started().session_uuid, perr,
                                                verbose_);
        if (state == StreamSession::ServerState::Alive) {
            reviving_ = false;
            if (verbose_) {
                std::printf("%s：%llu ms 未收到数据，设备报告会话仍运行，继续等待\n",
                            why, static_cast<unsigned long long>(quiet_ms));
            }
            return false;  // 设备报告会话仍运行，暂不重建。
        }
        std::printf("%s：%llu ms 未收到数据，%s，重建视频会话\n", why,
                    static_cast<unsigned long long>(quiet_ms),
                    state == StreamSession::ServerState::Ended
                        ? "设备会话已结束"
                        : ("查询会话状态失败（" + perr + "）").c_str());
        std::string restart_err;
        if (!restart(restart_err)) {
            std::fprintf(stderr, "重建视频会话失败: %s\n", restart_err.c_str());
            reviving_ = false;
            return false;
        }
        new_session_state();
        return true;
    };

    /// 直接重建会话并重置收流状态，省去状态查询 RPC 的等待。
    /// 用于超过静默阈值或需要重新取得关键帧的情况。
    auto restart_now = [&]() {
        std::string restart_err;
        if (!restart(restart_err)) {
            std::fprintf(stderr, "重建视频会话失败: %s\n", restart_err.c_str());
            return false;
        }
        new_session_state();
        return true;
    };

    /// 按最后一个数据报的时间处理静默，包括 RTCP SR：
    /// 静默不超过 1.2 秒时继续等待；介于 1.2 秒和 blind_at 之间时查询设备；
    /// 超过 blind_at 时直接重建。缺少 SR 可能是丢包，不能单独证明会话结束。
    /// ask_every_ms 限制中间区间的查询频率；用户 wake 传 0，不额外节流。
    uint64_t last_ask_ms_ = 0;
    auto judge_quiet = [&](uint64_t quiet, uint64_t blind_at, uint64_t ask_every_ms,
                           const char *why) {
        if (quiet > blind_at) {
            std::printf("%s：%llu ms 未收到数据，超过恢复阈值，重建视频会话\n",
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
        // restart() 失败会使 session_ 为空。统一在此重试并跳过后续会话访问；
        // 通过可取消的条件变量等待实现一秒退避，使停止请求能够及时退出。
        if (session_ == nullptr) {
            std::string rerr;
            if (restart(rerr)) {
                new_session_state();
                continue;
            }
            std::fprintf(stderr, "重建视频会话失败: %s（1 秒后再试）\n", rerr.c_str());
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
            if (stopping_) {
                return;
            }
            continue;
        }
        // 每秒发送 RR，延长设备 RTCPTimeoutInterval 定义的空闲租期。
        // 调度置于每轮循环，不能依赖读包超时，否则连续视频包会阻止发送。
        // 沿用真机验证的 32 字节裸 RR：发送者为 answer.RemoteSSRC，
        // 报告块指向设备 LocalSSRC，发至 sender.port（已验证与 SourcePort 一致）。
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
                // 每个会话只输出第一次发送失败，避免重复日志掩盖恢复结果。
                std::fprintf(stderr, "RTCP 保活包发送失败: %s\n", serr.c_str());
            }
        }
        // 收到取帧或输入触发的 wake 请求后，按相同静默策略检查会话。
        if (wake_requested_.exchange(false)) {
            judge_quiet(now_ms() - last_packet_ms_, kQuietCertainMs, 0, "收到操作");
            continue;
        }
        // 降级重试计时每轮检查，不依赖收到超大 AU 或发生读超时。
        // 持续收包和完全无包两种情况都必须能执行定时恢复。
        if (video_unusable_ && now_ms() - last_restart_ms_ >= kOversizedRetryMs) {
            last_restart_ms_ = now_ms();
            oversized_restart_ = true;
            std::printf("视频降级已满 %llu 秒，重试视频会话"
                        "\n",
                        static_cast<unsigned long long>(kOversizedRetryMs / 1000));
        }
        if (oversized_restart_) {
            oversized_restart_ = false;
            // 降级既可能来自尺寸限制，也可能来自连续无关键帧输出，日志不能
            // 将定时重试一律归因于后端尺寸限制。
            std::printf("%s，重建视频会话以取得关键帧\n",
                        video_unusable_ ? "视频降级后的定时重试"
                                        : "NAL 超过平台后端长度上限");
            // 失败后的空会话由循环顶部统一退避重试。
            restart_now();
            continue;
        }
        // PLI 后的后备重建每轮检查，即使只收到 SR 而没有视频字节也要推进
        // 等待计时。条件同时要求丢包后的等待未结束及关键帧等待达到期限。
        if (options_.stall_restart_ms > 0 && depacketizer != nullptr) {
            const auto &dst = depacketizer->stats();
            const uint64_t gaps = dst.seq_gaps;
            bool stalled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // awaiting_idr_from_loss_ 表示丢包后的等待，成功解码关键帧才清除；
                // first_pli_ms_ 是首次请求时间，重发不延后期限；last_keyframe_ms_
                // 记录最近解析到关键帧 AU 的时间，避免刚收到关键帧便重建。
                // 不能以本轮新增缺口代替持续等待：缺口发生时超时尚未到达，
                // 期限到达时又不再有新缺口，会使两项条件永远无法同时成立。
                stalled = awaiting_idr_from_loss_ && first_pli_ms_ != 0 &&
                          now_ms() - first_pli_ms_ >=
                              static_cast<uint64_t>(options_.stall_restart_ms) &&
                          now_ms() - last_keyframe_ms_ >
                              static_cast<uint64_t>(options_.stall_restart_ms);
                stats_.gaps = gaps;
                stats_.dropped_fragments = dst.dropped_fragments;
                if (stalled) {
                    last_keyframe_ms_ = now_ms();  // 为新会话留出等待时间。
                }
            }
            if (stalled) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.stall_restarts;
                }
                std::printf("等待关键帧达到 %d ms，重建视频会话\n",
                            options_.stall_restart_ms);
                restart_now();  // 失败由循环顶端的空会话兜底接手退避重试
                continue;       // 这一轮的包属于上一条会话了
            }
        }

        // 启动后无关键帧输出时按 plan_nokey 执行有限快速重试，再转定时重试。
        // 此计时每轮检查，不能只放在 next_packet 超时分支：连续收包但解码
        // 无输出时仍需要恢复。实测回归过程见 docs/coredevice.md §20。
        if (!ever_keyframe_ && now_ms() - session_start_ms_ > kNokeyBlindMs) {
            switch (plan_nokey(nokey_restarts_, kMaxNokeyRestarts, video_unusable_)) {
            case NokeyAction::kRetry:
                ++nokey_restarts_;
                session_start_ms_ = now_ms();
                std::printf("启动 %llu 秒仍无关键帧输出，重建视频会话 (%d/%d)\n",
                            static_cast<unsigned long long>(kNokeyBlindMs / 1000),
                            nokey_restarts_, kMaxNokeyRestarts);
                restart_now();
                continue;  // 这一轮的包属于上一条会话了
            case NokeyAction::kDegrade:
                video_unusable_ = true;
                std::printf("连续 %d 次重建仍未输出关键帧，视频暂不可用。"
                            "调用方可切换至截图服务；将每 %llu 秒重试视频会话。"
                            "\n",
                            kMaxNokeyRestarts,
                            static_cast<unsigned long long>(kOversizedRetryMs / 1000));
                break;
            case NokeyAction::kWait:
                break;
            }
            // 降级重试由上方 video_unusable_ 计时统一调度。
        }

        if (!session_->next_packet(datagram, 50, err)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            // 接收超时或失败时按静默时长判断恢复，不凭一次接收失败认定会话结束。
            if (options_.silence_restart_ms > 0) {
                const uint64_t quiet = now_ms() - last_packet_ms_;
                judge_quiet(quiet, std::max<uint64_t>(kQuietCertainMs,
                                                       static_cast<uint64_t>(options_.silence_restart_ms)),
                            kSrPeriodMs, "静默超时");
            }
            continue;  // 超时不是结束
        }
        last_packet_ms_ = now_ms();
        // 视频端口同时接收裸 RTCP SR。is_rtcp_sr 校验后读取偏移 20 和 24
        // 的设备累计视频包数与字节数；这些值只反映最后一份 SR 的时刻。
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

        if (!is_sr) {
            ++video_seen;
        }
        if (options_.debug_drop_nth_packet > 0 && !is_sr &&
            static_cast<int>(video_seen) == options_.debug_drop_nth_packet) {
            std::printf("测试：丢弃第 %d 个视频包以注入序号缺口\n",
                        options_.debug_drop_nth_packet);
            options_.debug_drop_nth_packet = 0;  // 一次性：重起之后的新会话不该再被丢
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.debug_dropped_at_ms = now_ms();  // 记录故障注入时间，供测试检查恢复期限。
            continue;
        }
        if (options_.debug_ignore_video_after > 0 && !is_sr &&
            static_cast<int>(video_seen) > options_.debug_ignore_video_after) {
            // 测试模式忽略超过指定计数的视频包，仅保留 SR，用于验证持续收到
            // RTCP 但无视频输出时恢复计时仍能触发。
            continue;
        }
        std::vector<uint8_t> bytes;
        const uint64_t t_dp0 = now_ms();
        const bool pushed = depacketizer->push(datagram, bytes, err);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.other_payload = depacketizer->stats().other_payload;
            stats_.ms_depacketize += static_cast<double>(now_ms() - t_dp0);
        }
        note_loss();
        if (!pushed || bytes.empty()) {
            continue;
        }
        if (record_ != nullptr) {
            std::fwrite(bytes.data(), 1, bytes.size(), record_);
        }
        parser_->feed(bytes.data(), bytes.size());

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
