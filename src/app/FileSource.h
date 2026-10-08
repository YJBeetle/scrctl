#pragma once

#include "app/FrameSource.h"
#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"
#include <array>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scrctl::app {

/// Annex-B 文件画面源。同步读取固定块，解析回调只保存编码 AU；next 按顺序
/// 解码一个 AU，不在解析块时积压 BGRA 图像。编码块及跨块 NAL/AU 仍占用内存，
/// 这里不承诺异常大 NAL 或整个进程的内存上限。
class FileSource final : public FrameSource {
  public:
    /// 默认使用软件工厂；工厂未编入时明确提示并回退平台后端，保留无 libav 的
    /// Apple 构建能力。显式 hardware 只选平台工厂，已创建后端的配置失败不回退。
    explicit FileSource(std::string path, bool use_hardware = false)
        : path_(std::move(path)), use_hardware_(use_hardware) {}

    bool next(scrctl::Frame &out, int timeout_ms) override;

    [[nodiscard]] bool finished() const override { return done_; }
    [[nodiscard]] bool failed() const override { return !error_.empty(); }
    [[nodiscard]] std::string end_reason() const override {
        return failed() ? error_ : FrameSource::end_reason();
    }
    [[nodiscard]] bool paces_itself() const override { return true; }

  private:
    bool open_file();
    bool read_block();
    bool decode_au(std::vector<scrctl::Nal> &&au, scrctl::Frame &out);
    void finish_input();
    void fail(std::string reason);

    static constexpr std::size_t kChunk = 48 * 1024;
    std::string path_;
    std::string error_;
    std::unique_ptr<FILE, int (*)(FILE *)> input_{nullptr, std::fclose};
    std::array<uint8_t, kChunk> buffer_{};
    std::deque<std::vector<scrctl::Nal>> access_units_;
    std::unique_ptr<scrctl::Decoder> decoder_;
    std::unique_ptr<scrctl::AnnexBParser> parser_;
    // 随 AU 的消费顺序更新，避免使用解析器已被后续图像覆盖的参数缓存。
    scrctl::Nal vps_, sps_, pps_;
    bool use_hardware_ = false;
    bool opened_ = false;
    bool configured_ = false;
    bool input_ended_ = false;
    bool produced_frame_ = false;
    bool done_ = false;
};

} // namespace scrctl::app
