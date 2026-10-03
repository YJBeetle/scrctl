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

/// 已录制的 Annex-B 文件。
///
/// 解析器是同步的——一次性 feed 整个文件会在任何一帧画出来之前就把上千帧全解进
/// 内存（每帧 11MB）。所以分块喂，并且解码结果攒在一个有上限的队列里。
class FileSource final : public FrameSource {
  public:
    explicit FileSource(std::string path) : path_(std::move(path)) {}

    bool next(scrctl::Frame &out, int timeout_ms) override;

    [[nodiscard]] bool finished() const override { return done_ && frames_.empty(); }
    [[nodiscard]] bool paces_itself() const override { return true; }

  private:
    /// 读一块、喂给解析器、让回调往队列里放帧。队列满了就停手，下次再喂。
    bool pump_bytes(std::string &err);

    bool open_file(std::string &err);

    void on_au(std::vector<scrctl::Nal> &&au);

    static constexpr std::size_t kChunk = 48 * 1024;
    /// 队列上限。一帧 11MB，攒太多只是把内存吃掉而画面并不会更连贯。
    static constexpr std::size_t kMaxQueued = 8;

    std::string path_;
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
