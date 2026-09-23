// scrctl：把设备屏幕镜像到原生窗口。
//
// 两种源：真机实时流（默认，不带 --play 时）与已录制的 Annex-B 文件。两条路
// 共用同一个喂字节 -> 拆 AU -> 解码 -> 渲染的循环，区别只在"字节从哪来"。
//
// 解析器是同步的——一次性 feed 整个文件会在任何一帧被画出来之前就把上千帧
// 全解进内存（每帧 11MB）。所以分块喂，并用帧队列做背压。实时源同理：一次
// 只取一个数据报，喂完就回到事件循环，否则窗口会在等帧的时候冻住。
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

struct Options {
    std::string path;       ///< 空 = 走真机实时流
    std::string serial;     ///< scrcpy 的 --serial：指定哪台设备
    std::string record;     ///< 实时流顺手把 Annex-B 录到文件
    bool list_devices = false;
    std::string title = "scrctl";
    bool stats = false;
    bool crop_set = false;
    int crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
    double scale = 1.0;   ///< 窗口相对裁剪尺寸的缩放
    int exit_after = 0;   ///< 渲染多少帧后退出（0=不限）
    int verify_at = 0;    ///< 渲染到第 N 帧时回读窗口内容
    std::string verify_path;
};

void usage(const char *argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "\n"
        "  (无参数)             镜像当前连接的设备\n"
        "  --serial SERIAL      多台设备时指定哪一台（UDID）\n"
        "  --list-devices       列出在连设备后退出\n"
        "  --play FILE          改播已录制的 Annex-B HEVC 文件\n"
        "  --record FILE        把实时流另存为 Annex-B\n"
        "  --crop WxH+X+Y       裁剪区域（默认自动：1136x2464 -> 1125x2436）\n"
        "  --scale F            窗口缩放系数，默认 1.0\n"
        "  --title TITLE        窗口标题\n"
        "  --stats              每秒打印帧率统计\n"
        "  --exit-after N       渲染 N 帧后退出\n"
        "  --verify N FILE      渲染到第 N 帧时把窗口内容回读存为 BMP\n",
        argv0);
}

bool parse_args(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺少参数值\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--play") {
            o.path = next("--play");
        } else if (a == "--serial") {
            o.serial = next("--serial");
        } else if (a == "--record") {
            o.record = next("--record");
        } else if (a == "--list-devices") {
            o.list_devices = true;
        } else if (a == "--title") {
            o.title = next("--title");
        } else if (a == "--scale") {
            o.scale = std::atof(next("--scale"));
        } else if (a == "--stats") {
            o.stats = true;
        } else if (a == "--exit-after") {
            o.exit_after = std::atoi(next("--exit-after"));
        } else if (a == "--verify") {
            o.verify_at = std::atoi(next("--verify"));
            o.verify_path = next("--verify");
        } else if (a == "--crop") {
            const char *v = next("--crop");
            if (std::sscanf(v, "%dx%d+%d+%d", &o.crop_w, &o.crop_h, &o.crop_x, &o.crop_y) != 4) {
                std::fprintf(stderr, "--crop 格式应为 WxH+X+Y，收到 %s\n", v);
                return false;
            }
            o.crop_set = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "未知参数 %s\n", a.c_str());
            return false;
        }
    }
    if (o.scale <= 0.0) {
        o.scale = 1.0;
    }
    return true;
}

/// 设备编码分辨率比逻辑显示大（HEVC CTU 对齐填充）。实测 iPhone 13 mini
/// 编码 1136x2464、逻辑显示 1125x2436，右侧 11px 与底部 28px 是垃圾像素。
/// 触摸归一化必须用逻辑尺寸，所以这里也按逻辑尺寸裁。
struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
};

Crop resolve_crop(const Options &o, const scrctl::Frame &f) {
    Crop c;
    if (o.crop_set) {
        c = {o.crop_x, o.crop_y, o.crop_w, o.crop_h};
    } else if (f.width == 1136 && f.height == 2464) {
        c = {0, 0, 1125, 2436};
        std::printf("自动裁剪 1136x2464 -> 1125x2436（CTU 填充：右 11px / 底 28px）\n");
    } else {
        c = {0, 0, static_cast<int>(f.width), static_cast<int>(f.height)};
    }
    c.x = std::max(0, std::min(c.x, static_cast<int>(f.width) - 1));
    c.y = std::max(0, std::min(c.y, static_cast<int>(f.height) - 1));
    c.w = std::max(1, std::min(c.w, static_cast<int>(f.width) - c.x));
    c.h = std::max(1, std::min(c.h, static_cast<int>(f.height) - c.y));
    return c;
}

class Presenter {
public:
    bool open(int frame_w, int frame_h, const Crop &crop, double scale, const std::string &title,
              bool want_readback) {
        win_w_ = static_cast<int>(crop.w * scale);
        win_h_ = static_cast<int>(crop.h * scale);
        src_ = crop;

        window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   win_w_, win_h_, SDL_WINDOW_ALLOW_HIGHDPI);
        if (window_ == nullptr) {
            std::fprintf(stderr, "建窗口失败: %s\n", SDL_GetError());
            return false;
        }
        // SDL2 的 Metal 后端不支持 SDL_RenderReadPixels——需要回读验证时
        // 直接建软件渲染器，否则 Present 后读回会无声 abort。
        const Uint32 flags =
            want_readback ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED;
        renderer_ = SDL_CreateRenderer(window_, -1, flags);
        if (renderer_ == nullptr && flags != SDL_RENDERER_SOFTWARE) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (renderer_ == nullptr) {
            std::fprintf(stderr, "建渲染器失败: %s\n", SDL_GetError());
            return false;
        }
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(renderer_, &info) == 0) {
            std::printf("渲染驱动: %s\n", info.name);
        }
        // 纹理必须是**源帧尺寸**——整帧上传进按裁剪尺寸建的纹理会因尺寸不符
        // 而失败。裁剪与缩放统一交给 RenderCopy 的 src/dst 矩形表达。
        // BGRA 内存布局对应 little-endian 的 ARGB8888。
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
        if (texture_ == nullptr) {
            std::fprintf(stderr, "建纹理失败: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetTextureScaleMode(texture_, SDL_ScaleModeBest);
        std::printf("窗口 %dx%d（源帧 %dx%d，裁剪 %dx%d+%d+%d，缩放 %.2f）\n", win_w_, win_h_,
                    frame_w, frame_h, crop.w, crop.h, crop.x, crop.y, scale);
        return true;
    }

    void draw(const scrctl::Frame &f, const char *readback_path = nullptr) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (SDL_UpdateTexture(texture_, nullptr, f.pixels.data(), static_cast<int>(f.row_pitch)) !=
            0) {
            std::fprintf(stderr, "上传纹理失败: %s\n", SDL_GetError());
        }
        const SDL_Rect src{src_.x, src_.y, src_.w, src_.h};
        const SDL_Rect dst{0, 0, win_w_, win_h_};
        SDL_RenderCopy(renderer_, texture_, &src, &dst);
        // 必须在 Present 之前读：Present 之后后缓冲已交换，SDL_RenderReadPixels
        // 会读到失效内容并段错误。
        if (readback_path != nullptr) {
            readback(readback_path);
        }
        SDL_RenderPresent(renderer_);
    }

    /// 把真正呈现到窗口上的内容读回存盘。日志只能证明帧率，证明不了画面；
    /// 而裁剪/缩放/纹理尺寸这类错误恰恰只有回读才看得见。
    bool readback(const std::string &path) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, win_w_, win_h_, 32,
                                                        SDL_PIXELFORMAT_ARGB8888);
        if (s == nullptr) {
            std::fprintf(stderr, "回读建面失败: %s\n", SDL_GetError());
            return false;
        }
        if (SDL_LockSurface(s) != 0) {
            std::fprintf(stderr, "回读加锁失败: %s\n", SDL_GetError());
            SDL_FreeSurface(s);
            return false;
        }
        // 显式给矩形：SDL2 的 software 驱动在 rect=NULL 时会段错误（实测）。
        const SDL_Rect full{0, 0, win_w_, win_h_};
        const int rc = SDL_RenderReadPixels(renderer_, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels,
                                            s->pitch);
        SDL_UnlockSurface(s);
        if (rc != 0) {
            std::fprintf(stderr, "回读失败: %s\n", SDL_GetError());
            SDL_FreeSurface(s);
            return false;
        }
        const int save = SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
        if (save != 0) {
            std::fprintf(stderr, "存图失败: %s\n", SDL_GetError());
            return false;
        }
        std::printf("已回读窗口内容 -> %s (%dx%d)\n", path.c_str(), win_w_, win_h_);
        return true;
    }

    bool pump_and_should_quit() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                return true;
            }
            if (e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)) {
                return true;
            }
        }
        return false;
    }

    ~Presenter() {
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
        }
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
    }

private:
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    Crop src_{};
    int win_w_ = 0, win_h_ = 0;
};

// ------------------------------------------------------------------ 字节源 ----

/// 一次 pull 取回一段可以喂给 AnnexBParser 的字节。
///
/// 抽象成"取一段"而不是"取一帧"，是因为实时与文件在帧边界上表现完全不同：
/// 文件可以一口气喂 48 KB，实时只能来一个数据报喂一个，否则窗口要等。
class Source {
public:
    virtual ~Source() = default;
    /// 返回 false 且 finished() 为真表示源已结束；返回 false 但未结束表示
    /// 这次没取到（超时），调用方应继续跑事件循环。实现可以阻塞一小会儿再返回
    /// false——实时源就是靠这个把"等包"和"空转"分开的。
    virtual bool pull(std::vector<uint8_t> &out, std::string &err) = 0;
    [[nodiscard]] virtual bool finished() const = 0;
    /// 文件回放要按标称帧率节流；实时源本身就是节拍，不能自己再等。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
};

class FileSource : public Source {
public:
    explicit FileSource(std::string path, std::size_t chunk = 48 * 1024)
        : path_(std::move(path)), chunk_(chunk) {}

    bool pull(std::vector<uint8_t> &out, std::string &err) override {
        if (!opened_ && !open_file(err)) {
            return false;
        }
        if (buffer_.empty()) {
            done_ = true;
            return false;
        }
        const std::size_t n = std::min(chunk_, buffer_.size());
        out.assign(buffer_.begin(), buffer_.begin() + static_cast<long>(n));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(n));
        if (buffer_.empty()) {
            done_ = true;
        }
        return true;
    }

    [[nodiscard]] bool finished() const override { return done_; }
    [[nodiscard]] bool paces_itself() const override { return true; }

private:
    bool open_file(std::string &err) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            err = "打不开 " + path_;
            return false;
        }
        buffer_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        opened_ = true;
        std::printf("读入 %s (%zu 字节)\n", path_.c_str(), buffer_.size());
        return true;
    }

    std::string path_;
    std::size_t chunk_;
    std::vector<uint8_t> buffer_;
    bool opened_ = false;
    bool done_ = false;
};

/// 真机实时流：起流 -> 收 RTP -> 拆包成 Annex-B。
///
/// 收包必须在**自己的线程**上跑。渲染一帧要几十毫秒，这期间不把隧道读干净，
/// 设备侧的中继缓冲就会溢出并**静默丢包**——它的序号照样连续（丢在编号之前），
/// 于是我们收到一个"完整"但内容被截断的关键帧，画面顶部对、下面全糊。
/// 实测正是这个症状：单线程版录出来的流第 2 帧就是噪声。
///
/// 反方向同理：pull 会阻塞等一小会儿，而不是"没有就立刻返回"。否则消费循环在
/// 两个数据报之间空转，把一颗核吃满，还给窗口饿出事件事件——窗口看起来就是卡住。
class LiveSource : public Source {
public:
    ~LiveSource() override;

    bool start(const std::string &serial, const std::string &record_path, std::string &err);

    bool pull(std::vector<uint8_t> &out, std::string & /*err*/) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(100),
                          [&] { return !chunks_.empty() || stopping_; })) {
            return false;  // 这一会儿没到：不是结束，调用方去泵一遍事件循环
        }
        if (chunks_.empty()) {
            return false;  // 正在关闭
        }
        out = std::move(chunks_.front());
        chunks_.pop_front();
        buffered_ -= out.size();
        return true;
    }

    [[nodiscard]] bool finished() const override { return false; }

    /// 拆包统计。画面糊掉时第一个要看的数就是这里。
    /// 读的是工作线程发布的快照，不是直接读它的状态——拆包器只归那个线程碰。
    void print_stats() const {
        const auto [st, overflow] = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return std::pair{snapshot_, overflow_};
        }();
        std::printf("  RTP: 包 %llu NAL %llu 序号断流 %llu 次/丢 %llu 包 乱序 %llu  "
                    "丢半成品 %llu 畸形 %llu 非视频包 %llu 溢出丢弃 %zu 块\n",
                    static_cast<unsigned long long>(st.packets),
                    static_cast<unsigned long long>(st.nals),
                    static_cast<unsigned long long>(st.seq_gaps),
                    static_cast<unsigned long long>(st.seq_lost),
                    static_cast<unsigned long long>(st.reordered),
                    static_cast<unsigned long long>(st.dropped_fragments),
                    static_cast<unsigned long long>(st.malformed),
                    static_cast<unsigned long long>(st.other_payload), overflow);
    }

private:
    void receive_loop();

    std::thread worker_;
    std::atomic<bool> stopping_{false};
    /// print_stats() 是 const 的，要能在只读路径上取快照。
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> chunks_;
    /// 背压上限。超了只能丢，同时记数——丢了就得去要关键帧（M2.7 的 PLI）。
    static constexpr std::size_t kMaxBuffered = 32u << 20;
    std::size_t buffered_ = 0;
    std::size_t overflow_ = 0;
    scrctl::rt::HevcRtpDepacketizer::Stats snapshot_{};
    std::unique_ptr<scrctl::remote::Device> device_;
    std::unique_ptr<scrctl::media::StreamSession> session_;
    /// PT 是协商出来的，构造时还不知道，所以在 start() 里赋值。
    scrctl::rt::HevcRtpDepacketizer depacketizer_{100};
    FILE *record_ = nullptr;
};

LiveSource::~LiveSource() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (record_ != nullptr) {
        std::fclose(record_);
        record_ = nullptr;
    }
}

void LiveSource::receive_loop() {
    std::string err;
    std::vector<uint8_t> datagram;
    while (!stopping_) {
        if (!session_->next_packet(datagram, 50, err)) {
            continue;  // 超时不是结束，接着等
        }
        std::vector<uint8_t> bytes;
        if (!depacketizer_.push(datagram, bytes, err) || bytes.empty()) {
            continue;
        }
        if (record_ != nullptr) {
            std::fwrite(bytes.data(), 1, bytes.size(), record_);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = depacketizer_.stats();
            if (buffered_ > kMaxBuffered) {
                ++overflow_;
                continue;  // 消费者跟不上：丢掉，而不是把内存吃光
            }
            buffered_ += bytes.size();
            chunks_.push_back(std::move(bytes));
        }
        cv_.notify_all();
    }
}

bool LiveSource::start(const std::string &serial, const std::string &record_path,
                       std::string &err) {
    auto dev = scrctl::remote::Device::establish(serial, err);
    if (!dev) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));
    scrctl::media::StreamSession::Request request;
    session_ = scrctl::media::StreamSession::start(*device_, request, err);
    if (session_ == nullptr) {
        return false;
    }
    // RTCP 与视频共用这个 UDP 端口，只能靠 PT 分辨；不过滤的话 RTCP 会被当成
    // HEVC 载荷解出假 NAL，把参考链一路带坏。
    depacketizer_ = scrctl::rt::HevcRtpDepacketizer{session_->started().payload_type};
    std::printf("流已建立：%s / iOS %s，收流端口=%u PT=%u\n",
                device_->property("ProductType").c_str(), device_->property("OSVersion").c_str(),
                session_->receiver_port(), session_->started().payload_type);
    if (!record_path.empty()) {
        record_ = std::fopen(record_path.c_str(), "wb");
        if (record_ == nullptr) {
            err = "打不开录制文件 " + record_path;
            return false;
        }
        std::printf("录制到 %s\n", record_path.c_str());
    }
    worker_ = std::thread(&LiveSource::receive_loop, this);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    // 崩溃时块缓冲的 stdout 会整段丢失，导致"无输出"无法定位。诊断工具
    // 的输出量很小，直接无缓冲。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    if (!parse_args(argc, argv, o)) {
        usage(argv[0]);
        return 2;
    }

    if (o.list_devices) {
        std::string err;
        auto devices = scrctl::remote::Device::list(err);
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (const auto &d : devices) {
            std::printf("%s  %s\n", d.udid.c_str(), d.connection_type.c_str());
        }
        return 0;
    }

    std::unique_ptr<Source> source;
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path);
    } else {
        auto live = std::make_unique<LiveSource>();
        std::string err;
        if (!live->start(o.serial, o.record, err)) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            return 1;
        }
        source = std::move(live);
    }

    auto decoder = scrctl::create_platform_decoder();
    std::printf("解码后端: %s\n", decoder->backend_name());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    int rendered = 0, decoded = 0, failed = 0;
    bool configured = false;
    std::deque<scrctl::Frame> queue;

    scrctl::AnnexBParser parser([&](std::vector<scrctl::Nal> &&au, bool) {
        if (!configured) {
            scrctl::Nal vps, sps, pps;
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
        scrctl::Frame f;
        if (decoder->decode(au, f) && f) {
            ++decoded;
            queue.push_back(std::move(f));
        } else {
            ++failed;
        }
    });

    std::unique_ptr<Presenter> presenter;
    const Uint64 start = SDL_GetTicks64();
    int last_reported = 0;
    std::string pull_err;
    bool quit = false;

    while (!quit) {
        if (queue.empty()) {
            // 没帧可画就去取一段。取不到（实时源的 50ms 超时）不是结束，
            // 必须继续往下走一遍事件循环，否则等帧的时候窗口会整个冻住。
            std::vector<uint8_t> bytes;
            if (!source->pull(bytes, pull_err) && source->finished()) {
                // 收尾必须 flush：AU 的边界靠"下一个图像的起始 slice"判定，最后一个
                // AU 没有下一个，不 flush 就永远等不到它。
                parser.flush();
                if (queue.empty()) {
                    std::printf("源已结束\n");
                    break;
                }
            }
            if (!bytes.empty()) {
                parser.feed(bytes.data(), bytes.size());
            }
        }
        if (queue.empty()) {
            // 没帧可画也要让窗口活着——此刻基本都是在等实时包到达，而等帧的时候
            // 不泵事件，窗口就是"未响应"。
            if (presenter != nullptr) {
                quit = presenter->pump_and_should_quit();
            }
            continue;
        }

        scrctl::Frame f = std::move(queue.front());
        queue.pop_front();

        if (presenter == nullptr) {
            presenter = std::make_unique<Presenter>();
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 resolve_crop(o, f), o.scale, o.title, o.verify_at > 0)) {
                return 1;
            }
        }

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        presenter->draw(f, do_verify ? o.verify_path.c_str() : nullptr);
        ++rendered;

        // 只有文件回放需要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
        if (source->paces_itself()) {
            const Uint64 want_ms = static_cast<Uint64>(rendered) * 1000 / 60;
            const Uint64 now = SDL_GetTicks64() - start;
            if (want_ms > now) {
                SDL_Delay(static_cast<Uint32>(want_ms - now));
            }
        }

        if (o.stats && rendered - last_reported >= 60) {
            const double el = (SDL_GetTicks64() - start) / 1000.0;
            std::printf("  渲染 %d 帧  %.1f fps  (解码 %d / 未出帧 %d)\n", rendered, rendered / el,
                        decoded, failed);
            if (auto *live = dynamic_cast<LiveSource *>(source.get())) {
                live->print_stats();
            }
            last_reported = rendered;
        }
        if (o.exit_after > 0 && rendered >= o.exit_after) {
            std::printf("达到 --exit-after %d\n", o.exit_after);
            break;
        }
        quit = presenter->pump_and_should_quit();
    }

    std::printf("完成：渲染 %d 帧，解码 %d 帧，未出帧 %d 帧\n", rendered, decoded, failed);
    presenter.reset();
    SDL_Quit();
    return 0;
}
