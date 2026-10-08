#include "i18n/Translation.h"
#include "app/FileSource.h"

#include <cstdio>
#include <filesystem>

namespace scrctl::app {

bool FileSource::next(scrctl::Frame &out, int timeout_ms) {
    // 普通本地文件同步读取；每次最多读一块或解码一个 AU，返回后调用方仍能
    // 处理窗口事件。decode 返回 false 可能只是暂无输出，不能据此终止回放。
    (void)timeout_ms;
    if (done_ || (!opened_ && !open_file())) {
        return false;
    }
    if (access_units_.empty() && !input_ended_ && !read_block()) {
        return false;
    }
    bool produced = false;
    if (!access_units_.empty()) {
        auto au = std::move(access_units_.front());
        access_units_.pop_front();
        produced = decode_au(std::move(au), out);
    }
    if (!done_ && input_ended_ && access_units_.empty()) {
        finish_input();
    }
    return produced;
}

bool FileSource::read_block() {
    const auto count = std::fread(buffer_.data(), 1, buffer_.size(), input_.get());
    // 短读可能是正常 EOF，也可能是磁盘/描述符错误，须读取 stdio 的错误状态。
    // libc++ 的 filebuf 可能把底层 fread 错误表现为 eofbit，单看 ifstream 状态
    // 无法可靠区分；这里直接使用同一标准库的 ferror/feof，不猜 errno 的含义。
    if (std::ferror(input_.get())) {
        fail(SCRCTL_TR("Cannot read playback file: ") + path_);
        return false;
    }
    if (count > 0 && !parser_->feed(buffer_.data(), count)) {
        fail(SCRCTL_TR("Cannot parse playback file: ") + path_);
        return false;
    }
    if (std::feof(input_.get())) {
        // 最后一幅图像没有下一 AU 为它定界，必须在真实 EOF 显式提交一次。
        parser_->flush();
        input_ended_ = true;
        input_.reset();
    }
    return true;
}

bool FileSource::open_file() {
    // --play 只消费普通磁盘文件。尤其 FIFO 的 fopen 本身可能等待 writer，
    // 不能把它交给忽略等待时限的同步文件路径；正常文件的符号链接仍可使用。
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(path_, status_error)) {
        fail((status_error ? SCRCTL_TR("Cannot open file ")
                           : SCRCTL_TR("Playback requires a regular file: ")) + path_);
        return false;
    }
    input_.reset(std::fopen(path_.c_str(), "rb"));
    if (!input_) {
        fail(SCRCTL_TR("Cannot open file ") + path_);
        return false;
    }
    bool platform_fallback = false;
    if (!use_hardware_) {
        decoder_ = scrctl::create_software_decoder();
        platform_fallback = decoder_ == nullptr;
    }
    if (decoder_ == nullptr) {
        decoder_ = scrctl::create_platform_decoder();
    }
    if (decoder_ == nullptr) {
        fail(SCRCTL_TR(scrctl::kNoDecoderMessage));
        return false;
    }
    if (platform_fallback) {
        std::fprintf(stderr, SCRCTL_TR(
            "Software decoder (libavcodec) is not included; using the platform decoder "
            "for file playback.\n"));
    }
    opened_ = true;
    std::printf(SCRCTL_TR("Playing file: %s\n"), path_.c_str());
    std::printf(SCRCTL_TR("Decoder backend: %s\n"), decoder_->backend_name());
    parser_ = std::make_unique<scrctl::AnnexBParser>(
        [this](std::vector<scrctl::Nal> &&au, bool) { access_units_.push_back(std::move(au)); });
    return true;
}

bool FileSource::decode_au(std::vector<scrctl::Nal> &&au, scrctl::Frame &out) {
    if (!configured_) {
        for (const auto &n : au) {
            if (n.size() < 2) {
                continue;
            }
            switch ((n[0] >> 1) & 0x3F) {
            case 32:
                vps_ = n;
                break;
            case 33:
                sps_ = n;
                break;
            case 34:
                pps_ = n;
                break;
            default:
                break;
            }
        }
        if (vps_.empty() || sps_.empty() || pps_.empty()) {
            return false;
        }
        if (!decoder_->configure(vps_, sps_, pps_)) {
            fail(SCRCTL_TR("Cannot configure the HEVC decoder for playback"));
            return false;
        }
        configured_ = true;
    }
    scrctl::Frame f;
    if (decoder_->decode(au, f) && f) {
        produced_frame_ = true;
        out = std::move(f);
        return true;
    }
    return false;
}

void FileSource::finish_input() {
    if (!configured_ && !parser_->has_parameter_sets()) {
        fail(SCRCTL_TR("Playback file has no complete HEVC parameter sets"));
    } else if (!produced_frame_) {
        fail(SCRCTL_TR("Playback file produced no video frames"));
    } else {
        done_ = true;
    }
}

void FileSource::fail(std::string reason) {
    if (error_.empty()) {
        error_ = std::move(reason);
    }
    done_ = true;
    access_units_.clear();
    input_.reset();
}

} // namespace scrctl::app
