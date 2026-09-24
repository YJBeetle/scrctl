#include "media/FramePump.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "bitstream/AnnexB.h"
#include "media/StreamSession.h"
#include "remote/Device.h"

namespace scrctl::media {
namespace {

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
                    return;
                }
                need_keyframe_ = false;
            }

            Frame f;
            if (!decoder->decode(au, f) || !f) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.no_output;
                return;
            }
            if (keyframe) {
                ever_keyframe_ = true;
                nokey_restarts_ = 0;  // 只有真解出关键帧才重置上限，防死循环
            }
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = std::move(f);
            width_ = static_cast<int>(frame_.width);
            height_ = static_cast<int>(frame_.height);
            ++serial_;
            ++stats_.decoded;
            cv_.notify_all();
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
        std::string perr;
        const auto state = StreamSession::probe(device_, session_->started().session_uuid, perr,
                                                verbose_);
        if (state == StreamSession::ServerState::Alive) {
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
            return false;
        }
        new_session_state();
        return true;
    };

    for (;;) {
        // 用户动了一下手，而流已经安静了 150ms 以上：立刻确认一次，不要等静默窗口
        // 到点。不等的话手感就是"点下去愣一下画面才动"——设备在画面静止约 3 秒后
        // 就把流结束了，而按静默判据最快也要 3 秒才发现。
        if (wake_requested_.exchange(false)) {
            const uint64_t quiet = now_ms() - last_packet_ms_;
            if (quiet > 150) {
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            ++stats_.packets;
        }

        std::vector<uint8_t> bytes;
        if (!depacketizer->push(datagram, bytes, err) || bytes.empty()) {
            continue;
        }
        if (record_ != nullptr) {
            std::fwrite(bytes.data(), 1, bytes.size(), record_);
        }
        parser_->feed(bytes.data(), bytes.size());

        // 卡住的判据要两条同时成立：只看序号缺口会误伤（丢一个分片也许下一帧
        // 就是关键帧），只看"多久没关键帧"又会在静止画面上白白重起。
        if (options_.stall_restart_ms > 0) {
            const uint64_t gaps = depacketizer->stats().seq_gaps;
            bool stalled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stalled = gaps > gaps_at_last_check_ &&
                          now_ms() - last_keyframe_ms_ >
                              static_cast<uint64_t>(options_.stall_restart_ms);
                gaps_at_last_check_ = gaps;
                stats_.gaps = gaps;
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
