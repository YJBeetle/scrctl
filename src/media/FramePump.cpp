#include "media/FramePump.h"

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

    auto decoder = create_platform_decoder();
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
            if (!configured) {
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
            Frame f;
            if (!decoder->decode(au, f) || !f) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.no_output;
                return;
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

    auto new_session_state = [&] {
        depacketizer = std::make_unique<scrctl::rt::HevcRtpDepacketizer>(session_->started().payload_type);
        configured = false;
        parser_ = make_parser();
    };
    new_session_state();

    for (;;) {
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
            if (options_.silence_restart_ms > 0 &&
                now_ms() - last_packet_ms_ > static_cast<uint64_t>(options_.silence_restart_ms)) {
                std::printf("已 %d ms 没收到任何包，重起媒体会话\n", options_.silence_restart_ms);
                last_packet_ms_ = now_ms();
                std::string restart_err;
                if (!restart(restart_err)) {
                    std::fprintf(stderr, "重起媒体会话失败: %s\n", restart_err.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                new_session_state();
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
