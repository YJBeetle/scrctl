#include "app/FileSource.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cstdio>
#include <fstream>

namespace scrctl::app {

bool FileSource::next(scrctl::Frame &out, int timeout_ms) {
    (void)timeout_ms; // 文件不会"等不到"，只会有"读完了"
    std::string err;
    while (frames_.empty() && !done_ && !pump_bytes(err)) {
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            done_ = true;
            return false;
        }
    }
    if (frames_.empty()) {
        return false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

bool FileSource::pump_bytes(std::string &err) {
    if (!opened_ && !open_file(err)) {
        return false;
    }
    if (pos_ >= buffer_.size()) {
        // 文件末尾需要 flush。AU 边界由下一幅图像的首个 slice 确定，最后一个 AU
        // 没有后续图像，必须显式提交。
        if (!flushed_) {
            flushed_ = true;
            parser_->flush();
            done_ = true;
        }
        return false;
    }
    if (frames_.size() >= kMaxQueued) {
        return true; // 解码帧队列达到上限时停止读入，等待调用方消费。
    }
    const std::size_t n = std::min(kChunk, buffer_.size() - pos_);
    parser_->feed(buffer_.data() + pos_, n);
    pos_ += n;
    return true;
}

bool FileSource::open_file(std::string &err) {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        err = "无法打开文件 " + path_;
        return false;
    }
    buffer_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    opened_ = true;
    std::printf("读入 %s (%zu 字节)\n", path_.c_str(), buffer_.size());

    decoder_ = scrctl::create_platform_decoder();
    if (decoder_ == nullptr) {
        // 确认解码器存在后再解析文件；on_au 会调用 decoder_->configure。
        err = scrctl::kNoDecoderMessage;
        return false;
    }
    std::printf("解码后端: %s\n", decoder_->backend_name());
    parser_ = std::make_unique<scrctl::AnnexBParser>(
        [this](std::vector<scrctl::Nal> &&au, bool) { this->on_au(std::move(au)); });
    return true;
}

void FileSource::on_au(std::vector<scrctl::Nal> &&au) {
    if (!configured_) {
        scrctl::Nal vps, sps, pps;
        for (const auto &n : au) {
            if (n.size() < 2) {
                continue;
            }
            switch ((n[0] >> 1) & 0x3F) {
            case 32:
                vps = n;
                break;
            case 33:
                sps = n;
                break;
            case 34:
                pps = n;
                break;
            default:
                break;
            }
        }
        if (vps.empty() || sps.empty() || pps.empty() || !decoder_->configure(vps, sps, pps)) {
            return;
        }
        configured_ = true;
    }
    scrctl::Frame f;
    if (decoder_->decode(au, f) && f) {
        frames_.push_back(std::move(f));
    }
}

} // namespace scrctl::app
