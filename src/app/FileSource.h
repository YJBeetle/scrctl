#pragma once

#include "app/FrameSource.h"
#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scrctl::app {

/// Annex-B 文件画面源。解析器同步执行，分块读入并限制解码队列，
/// 避免在首帧显示前解码整个文件。高分辨率 BGRA 帧可能各占约 11 MiB。
class FileSource final : public FrameSource {
  public:
    explicit FileSource(std::string path) : path_(std::move(path)) {}

    bool next(scrctl::Frame &out, int timeout_ms) override;

    [[nodiscard]] bool finished() const override { return done_ && frames_.empty(); }
    [[nodiscard]] bool failed() const override { return !error_.empty(); }
    [[nodiscard]] std::string end_reason() const override {
        return failed() ? error_ : FrameSource::end_reason();
    }
    [[nodiscard]] bool paces_itself() const override { return true; }

  private:
    /// 读入一块并交给解析器；回调将帧放入队列。队列满后暂停，下次再继续。
    bool pump_bytes(std::string &err);

    bool open_file(std::string &err);

    void on_au(std::vector<scrctl::Nal> &&au);

    static constexpr std::size_t kChunk = 48 * 1024;
    /// 解码帧队列上限，控制高分辨率回放的内存占用。
    static constexpr std::size_t kMaxQueued = 8;

    std::string path_;
    std::string error_;
    std::vector<uint8_t> buffer_;
    std::deque<scrctl::Frame> frames_;
    std::unique_ptr<scrctl::Decoder> decoder_;
    std::unique_ptr<scrctl::AnnexBParser> parser_;
    std::size_t pos_ = 0;
    bool opened_ = false;
    bool configured_ = false;
    bool flushed_ = false;
    bool done_ = false;
};

} // namespace scrctl::app
