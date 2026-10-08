// 使用真实 FileSource 和 AnnexBParser；只在解码器工厂边界控制输出与失败。
// 最小 NAL 夹具验证文件消费行为，不宣称这些字节能被真实 HEVC 解码器接受。
#include "app/FileSource.h"
#include "i18n/Translation.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int checks = 0;
int failures = 0;

void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

using scrctl::Nal;
using Au = std::vector<Nal>;

struct DecoderState {
    bool software_available = true;
    bool platform_available = true;
    bool configure_ok = true;
    bool output = true;
    std::size_t first_output = 1;
    int software_calls = 0;
    int platform_calls = 0;
    int configure_calls = 0;
    std::vector<Au> decoded;
    Au configuration;
};
DecoderState state;

class ControlledDecoder final : public scrctl::Decoder {
  public:
    explicit ControlledDecoder(const char *name) : name_(name) {}

    bool configure(const Nal &vps, const Nal &sps, const Nal &pps) override {
        ++state.configure_calls;
        state.configuration = {vps, sps, pps};
        return state.configure_ok;
    }

    bool decode(const Au &au, scrctl::Frame &out) override {
        state.decoded.push_back(au);
        if (!state.output || state.decoded.size() < state.first_output) {
            return false;
        }
        out.width = out.height = 1;
        out.row_pitch = 4;
        out.pixels.resize(4);
        const auto sequence = static_cast<uint32_t>(state.decoded.size());
        std::memcpy(out.pixels.data(), &sequence, sizeof(sequence));
        return true;
    }

    const char *backend_name() const override { return name_; }

  private:
    const char *name_;
};

struct Fixture {
    std::vector<uint8_t> bytes;
    std::vector<Au> aus;
};

void append(std::vector<uint8_t> &bytes, const Nal &nal) {
    bytes.insert(bytes.end(), {0, 0, 0, 1});
    bytes.insert(bytes.end(), nal.begin(), nal.end());
}

Au parameters(uint8_t version = 0x55) {
    return {{0x40, 1, version}, {0x42, 1, version}, {0x44, 1, version}};
}

Nal slice(std::size_t index) {
    // 非零编码序号，避免载荷中的偶然 Annex-B 起始码。
    return {0x26, 1, 0x80, static_cast<uint8_t>(index % 251 + 1),
            static_cast<uint8_t>(index / 251 % 251 + 1), 0x55};
}

Fixture fixture(std::size_t count) {
    Fixture result;
    for (std::size_t i = 0; i < count; ++i) {
        Au au = i == 0 ? parameters() : Au{};
        au.push_back(slice(i));
        for (const auto &nal : au) {
            append(result.bytes, nal);
        }
        result.aus.push_back(std::move(au));
    }
    return result;
}

void save(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(out), "write owned fixture");
}

std::vector<uint32_t> drain(scrctl::app::FileSource &source) {
    std::vector<uint32_t> result;
    for (int i = 0; i < 100000 && !source.finished(); ++i) {
        scrctl::Frame frame;
        const auto before = state.decoded.size();
        if (source.next(frame, 0)) {
            uint32_t sequence = 0;
            if (frame.pixels.size() >= sizeof(sequence)) {
                std::memcpy(&sequence, frame.pixels.data(), sizeof(sequence));
            }
            result.push_back(sequence);
        }
        if (state.decoded.size() > before + 1) {
            check(false, "each next decodes at most one AU");
            break;
        }
    }
    check(source.finished(), "source eventually finishes");
    return result;
}

void check_sticky_failure(scrctl::app::FileSource &source) {
    check(source.finished() && source.failed() && !source.end_reason().empty() &&
              source.end_reason() != "Source ended",
          "failure has terminal source state and a useful reason");
    const auto reason = source.end_reason();
    const auto decoded = state.decoded.size();
    const auto configured = state.configure_calls;
    scrctl::Frame frame;
    check(!source.next(frame, 0) && source.finished() && source.failed() &&
              source.end_reason() == reason && state.decoded.size() == decoded &&
              state.configure_calls == configured,
          "repeat next preserves first failure without decoder work");
}

void test_failures(const std::filesystem::path &directory) {
    const auto three = fixture(3);
    const auto path = directory / "three.hevc";
    save(path, three.bytes);

    state = {};
    state.configure_ok = false;
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source).empty() && state.configure_calls == 1 && state.decoded.empty(),
              "configuration failure is terminal without silent backend retry");
        check(state.platform_calls == 0, "existing software backend is not silently replaced");
        check_sticky_failure(source);
    }

    state = {};
    state.output = false;
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source).empty() && state.decoded == three.aus,
              "temporary no-output consumes all AU before EOF classification");
        check_sticky_failure(source);
    }

    const auto empty = directory / "empty.hevc";
    save(empty, {});
    state = {};
    {
        scrctl::app::FileSource source(empty.string());
        check(drain(source).empty() && state.configure_calls == 0,
              "empty input is unsuccessful playback");
        check_sticky_failure(source);
    }

    const auto no_parameters = directory / "no-parameters.hevc";
    std::vector<uint8_t> bytes;
    append(bytes, slice(0));
    save(no_parameters, bytes);
    state = {};
    {
        scrctl::app::FileSource source(no_parameters.string());
        check(drain(source).empty() && state.configure_calls == 0,
              "missing parameter sets fail without configuring a decoder");
        check_sticky_failure(source);
    }

    state = {};
    {
        scrctl::app::FileSource source(directory.string());
        check(drain(source).empty(), "directory cannot be played as an empty stream");
        check(source.end_reason().find("Playback requires a regular file") == 0 &&
                  state.software_calls == 0 && state.platform_calls == 0,
              "directory is rejected before decoder or input stream creation");
        check_sticky_failure(source);
    }

    const auto parameters_only = directory / "parameters-only.hevc";
    bytes.clear();
    for (const auto &nal : parameters()) {
        append(bytes, nal);
    }
    save(parameters_only, bytes);
    state = {};
    {
        scrctl::app::FileSource source(parameters_only.string());
        check(drain(source).empty() && state.configure_calls == 0 &&
                  source.end_reason() == "Playback file produced no video frames",
              "complete parameters without images report missing output, not missing parameters");
        check_sticky_failure(source);
    }

    state = {};
    {
        scrctl::app::FileSource source((directory / "missing.hevc").string());
        check(drain(source).empty() && state.software_calls == 0 && state.platform_calls == 0,
              "missing file fails before decoder creation");
        check_sticky_failure(source);
    }

    state = {};
    state.software_available = state.platform_available = false;
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source).empty() && state.software_calls == 1 && state.platform_calls == 1,
              "missing decoder factories fail explicitly");
        check_sticky_failure(source);
    }
}

void test_success_and_backend(const std::filesystem::path &directory) {
    const auto three = fixture(3);
    const auto path = directory / "success.hevc";
    save(path, three.bytes);
    state = {};
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source) == std::vector<uint32_t>({1, 2, 3}) &&
                  state.decoded == three.aus && !source.failed(),
              "all AU are delivered once and in order, including final flush");
        check(state.software_calls == 1 && state.platform_calls == 0,
              "default selects available software decoder");
        scrctl::Frame frame;
        check(!source.next(frame, 0) && state.decoded.size() == 3 && !source.failed(),
              "successful EOF stays stable and never flushes twice");
    }

    state = {};
    state.first_output = 3;
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source) == std::vector<uint32_t>({3}) && state.decoded == three.aus &&
                  !source.failed(),
              "two temporary no-output AU followed by a frame remain successful");
    }

    state = {};
    {
        scrctl::app::FileSource source(path.string(), true);
        check(drain(source).size() == 3 && !source.failed() && state.software_calls == 0 &&
                  state.platform_calls == 1,
              "explicit hardware option uses the platform factory");
    }

    state = {};
    state.software_available = false;
    {
        scrctl::app::FileSource source(path.string());
        check(drain(source).size() == 3 && !source.failed() && state.software_calls == 1 &&
                  state.platform_calls == 1,
              "absent software factory permits the announced platform fallback");
    }

    state = {};
    state.platform_available = false;
    {
        scrctl::app::FileSource source(path.string(), true);
        check(drain(source).empty() && state.software_calls == 0 && state.platform_calls == 1,
              "explicit hardware selection does not silently switch to software");
        check_sticky_failure(source);
    }
}

void test_dense_and_parameter_order(const std::filesystem::path &directory) {
    const auto dense = fixture(10000);
    const auto path = directory / "dense.hevc";
    save(path, dense.bytes);
    state = {};
    {
        scrctl::app::FileSource source(path.string());
        scrctl::Frame frame;
        check(source.next(frame, 0) && state.decoded.size() == 1,
              "dense first read decodes one AU rather than thousands of BGRA frames");
        const auto rest = drain(source);
        bool ordered = rest.size() == 9999;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            ordered = ordered && rest[i] == i + 2;
        }
        check(ordered && state.decoded == dense.aus && !source.failed(),
              "dense chunks never drop or reorder AU to limit decoded buffering");
    }

    Fixture changed;
    changed.aus = {parameters(0x55), parameters(0x66), {slice(2)}};
    changed.aus[0].push_back(slice(0));
    changed.aus[1].push_back(slice(1));
    for (const auto &au : changed.aus) {
        for (const auto &nal : au) {
            append(changed.bytes, nal);
        }
    }
    const auto changed_path = directory / "parameter-order.hevc";
    save(changed_path, changed.bytes);
    state = {};
    {
        scrctl::app::FileSource source(changed_path.string());
        scrctl::Frame frame;
        check(source.next(frame, 0) && state.configuration == parameters(0x55),
              "first queued AU config uses its own parameters, not future parser cache");
        check(drain(source).size() == 2 && !source.failed(), "queued later AU remain consumable");
    }

    Fixture split;
    split.aus = {{{0x40, 1, 0x55}, slice(0)},
                 {{0x42, 1, 0x55}, {0x44, 1, 0x55}, slice(1)}};
    for (const auto &au : split.aus) {
        for (const auto &nal : au) {
            append(split.bytes, nal);
        }
    }
    const auto split_path = directory / "partial-parameters.hevc";
    save(split_path, split.bytes);
    state = {};
    {
        scrctl::app::FileSource source(split_path.string());
        check(drain(source).size() == 1 && state.configuration == parameters() && !source.failed(),
              "parameters can accumulate in consumed AU order before initial configuration");
    }
}

void test_multichunk(const std::filesystem::path &directory) {
    auto large = fixture(2);
    large.aus[0].back().insert(large.aus[0].back().end(), 70000, 0x55);
    large.bytes.clear();
    for (const auto &au : large.aus) {
        for (const auto &nal : au) {
            append(large.bytes, nal);
        }
    }
    const auto path = directory / "large-nal.hevc";
    save(path, large.bytes);
    state = {};
    {
        scrctl::app::FileSource source(path.string());
        scrctl::Frame frame;
        check(!source.next(frame, 0) && !source.finished() && state.decoded.empty(),
              "a cross-block NAL yields an event opportunity before it becomes a complete AU");
        check(drain(source) == std::vector<uint32_t>({1, 2}) && state.decoded == large.aus &&
                  !source.failed(),
              "cross-block NAL bytes and final AU are preserved");
    }
}

#ifndef _WIN32
// 查找本测试独占路径的只读描述符，不更改 FileSource 或标准库实现。
int owned_descriptor(const std::filesystem::path &path) {
    struct stat expected{};
    if (::stat(path.c_str(), &expected) != 0) {
        return -1;
    }
    for (int fd = 3; fd < 4096; ++fd) {
        struct stat actual{};
        if (::fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev &&
            actual.st_ino == expected.st_ino && (::fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) {
            return fd;
        }
    }
    return -1;
}

void test_actual_read_boundary(const std::filesystem::path &directory) {
    const auto fifo = directory / "must-not-block.fifo";
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create an owned FIFO without a writer");
    state = {};
    {
        scrctl::app::FileSource source(fifo.string());
        scrctl::Frame frame;
        check(!source.next(frame, 0) && state.software_calls == 0 &&
                  source.end_reason().find("Playback requires a regular file") == 0,
              "FIFO is rejected before fopen can wait forever for a writer");
        check_sticky_failure(source);
    }

    const auto input = fixture(20000);
    const auto path = directory / "read-boundary.hevc";
    save(path, input.bytes);
    state = {};
    {
        scrctl::app::FileSource source(path.string());
        scrctl::Frame frame;
        check(source.next(frame, 0), "streaming file produces its first frame");
        const int fd = owned_descriptor(path);
        check(fd >= 0, "identify only the owned fixture input descriptor");
        if (fd >= 0) {
            const auto position = ::lseek(fd, 0, SEEK_CUR);
            std::printf("First frame: input position=%lld, file bytes=%zu, decoded AU=%zu\n",
                        static_cast<long long>(position), input.bytes.size(), state.decoded.size());
            check(position > 0 && static_cast<uint64_t>(position) < input.bytes.size() &&
                      position <= 48 * 1024 + 8192,
                  "first frame precedes whole-file read, with only a fixed-block read-ahead");
            // 只关闭本进程对本测试文件的描述符，让真实标准库后续 read 遇到 EBADF。
            // source 销毁前不再打开其它文件，避免描述符重用影响别的资源。
            check(::close(fd) == 0, "inject owned input descriptor read failure");
            (void)drain(source);
            std::printf("Read failure: decoded AU=%zu, reason=%s\n",
                        state.decoded.size(), source.end_reason().c_str());
            check(source.failed() && source.end_reason().find("Cannot read playback file") == 0,
                  "a real later read failure cannot be mistaken for EOF after successful frames");
            check_sticky_failure(source);
        }
    }

    const auto symlink = directory / "regular-link.hevc";
    std::error_code error;
    std::filesystem::create_symlink(path, symlink, error);
    check(!error, "create a symlink to the owned regular fixture");
    if (!error) {
        state = {};
        scrctl::app::FileSource source(symlink.string());
        check(drain(source).size() == input.aus.size() && !source.failed(),
              "symlink to a regular file remains playable");
    }
}
#endif
} // namespace

namespace scrctl {
std::unique_ptr<Decoder> create_software_decoder() {
    ++state.software_calls;
    return state.software_available ? std::make_unique<ControlledDecoder>("ControlledSoftware")
                                    : nullptr;
}
std::unique_ptr<Decoder> create_platform_decoder() {
    ++state.platform_calls;
    return state.platform_available ? std::make_unique<ControlledDecoder>("ControlledPlatform")
                                    : nullptr;
}
} // namespace scrctl

int main() {
    check(scrctl::i18n::initialize("en"), "use explicit English diagnostics for deterministic checks");
    const auto directory = std::filesystem::temp_directory_path() /
        ("scrctl-filesource-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    check(std::filesystem::create_directory(directory, error), "create owned temporary directory");
    if (error) {
        return 1;
    }
    test_failures(directory);
    test_success_and_backend(directory);
    test_dense_and_parameter_order(directory);
    test_multichunk(directory);
#ifndef _WIN32
    test_actual_read_boundary(directory);
#endif
    std::filesystem::remove_all(directory, error);
    check(!error, "remove only owned fixtures");
    std::printf("FileSource: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
