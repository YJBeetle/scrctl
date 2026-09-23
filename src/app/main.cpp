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
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/ViewGeom.h"
#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"
#include "hid/Hid.h"
#include "media/FramePump.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

struct Options {
    std::string path;       ///< 空 = 走真机实时流
    std::string serial;     ///< scrcpy 的 --serial：指定哪台设备
    std::string record;     ///< 实时流顺手把 Annex-B 录到文件
    bool list_devices = false;
    bool no_control = false;  ///< scrcpy 的 --no-control：只看不动
    std::string title = "scrctl";
    bool stats = false;
    bool crop_set = false;
    int crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
    double scale = 1.0;   ///< 窗口相对裁剪尺寸的缩放
    bool scale_given = false;  ///< 显式给过 --scale 就别再自动缩进屏幕
    int exit_after = 0;   ///< 渲染多少帧后退出（0=不限）
    int verify_at = 0;    ///< 渲染到第 N 帧时回读窗口内容
    std::string verify_path;
    /// 注入一条直线后退出：`--test-touch x0,y0,x1,y1`。
    /// 窗口与鼠标不在场时也要能验证输入通路，理由同 --verify：日志说"注入
    /// 调用返回成功"证明不了设备上真的收到了触摸。
    std::string test_touch;
    /// 起流后按一次硬件按键（home/lock/volup/voldn/mute），然后照常镜像。
    /// 按键效果是瞬时的，所以它要能和 --verify 组合：按完等第 N 帧回读窗口。
    std::string test_button;
    /// 起流后往设备敲一段 ASCII（要有文本框正获得焦点）。
    std::string test_type;
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
        "  --no-control         只显示不注入输入（默认下鼠标左键即触摸）\n"
        "  --crop WxH+X+Y       裁剪区域（默认自动：1136x2464 -> 1125x2436）\n"
        "  --scale F            窗口缩放系数，默认 1.0\n"
        "  --title TITLE        窗口标题\n"
        "  --stats              每秒打印帧率统计\n"
        "  --exit-after N       渲染 N 帧后退出\n"
        "  --verify N FILE      渲染到第 N 帧时把窗口内容回读存为 BMP\n"
        "  --test-touch X0,Y0,X1,Y1\n"
        "                     注入一条直线（归一化坐标）后退出，无需真鼠标\n"
        "  --test-button NAME 起流后按一次硬件按键（home/lock/volup/voldn/mute）\n"
        "  --test-type TEXT   起流后往设备敲一段 ASCII（需要已聚焦的文本框）\n"
        "                     再照常镜像，配 --verify 才能看见瞬时效果\n",
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
        } else if (a == "--no-control") {
            o.no_control = true;
        } else if (a == "--title") {
            o.title = next("--title");
        } else if (a == "--scale") {
            o.scale = std::atof(next("--scale"));
            o.scale_given = true;
        } else if (a == "--stats") {
            o.stats = true;
        } else if (a == "--exit-after") {
            o.exit_after = std::atoi(next("--exit-after"));
        } else if (a == "--verify") {
            o.verify_at = std::atoi(next("--verify"));
            o.verify_path = next("--verify");
        } else if (a == "--test-touch") {
            o.test_touch = next("--test-touch");
        } else if (a == "--test-button") {
            o.test_button = next("--test-button");
        } else if (a == "--test-type") {
            o.test_type = next("--test-type");
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

/// 设备编码分辨率比逻辑显示大（HEVC CTU 对齐填充），所以画面要按逻辑尺寸裁；
/// 裁剪框与坐标换算的几何在 ViewGeom.h，那里可以离线自检。
using scrctl::app::Crop;
using scrctl::app::display_fraction;

Crop resolve_crop(const Options &o, const scrctl::Frame &f) {
    const auto auto_crop = scrctl::media::display_crop(static_cast<int>(f.width),
                                                       static_cast<int>(f.height));
    Crop c;
    if (o.crop_set) {
        c = {o.crop_x, o.crop_y, o.crop_w, o.crop_h, o.crop_w, o.crop_h};
    } else {
        c = {auto_crop.x, auto_crop.y, auto_crop.w, auto_crop.h, auto_crop.w, auto_crop.h};
        if (static_cast<int>(f.width) != auto_crop.w || static_cast<int>(f.height) != auto_crop.h) {
            std::printf("自动裁剪 %ux%u -> %dx%d（CTU 填充）\n", f.width, f.height, auto_crop.w,
                        auto_crop.h);
        }
    }
    c.x = std::max(0, std::min(c.x, static_cast<int>(f.width) - 1));
    c.y = std::max(0, std::min(c.y, static_cast<int>(f.height) - 1));
    c.w = std::max(1, std::min(c.w, static_cast<int>(f.width) - c.x));
    c.h = std::max(1, std::min(c.h, static_cast<int>(f.height) - c.y));
    c.display_w = std::max(1, c.display_w);
    c.display_h = std::max(1, c.display_h);
    return c;
}

class Presenter {
public:
    bool open(int frame_w, int frame_h, const Crop &crop, double scale, bool scale_given,
              const std::string &title, bool want_readback) {
        src_ = crop;
        SDL_Rect desk{};
        if (SDL_GetDisplayBounds(0, &desk) != 0 || desk.w <= 0) {
            desk.w = win_w_fallback;
            desk.h = win_h_fallback;
        }
        // 留一条标题栏的余量，别让窗口刚好顶满屏幕。
        scrctl::app::fit_window(crop.w, crop.h, desk.w, desk.h - 60, scale, scale_given, win_w_, win_h_);
        if (!scale_given && win_w_ < crop.w) {
            std::printf("屏幕只有 %dx%d 点，窗口缩到 %dx%d（--scale 可覆盖）\n", desk.w, desk.h,
                        win_w_, win_h_);
        }

        window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   win_w_, win_h_, SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
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
        // 不设 logical size 的话，渲染器坐标就是**像素**尺寸，而 ALLOW_HIGHDPI 下
        // 像素是窗口的两倍——按窗口点数画过去，内容就只占左上四分之一。设了它，
        // SDL 自己处理 Retina 缩放与窗口拉伸后的等比留边。
        SDL_RenderSetLogicalSize(renderer_, crop.w, crop.h);
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
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
        std::printf("窗口 %dx%d 点 / 绘制面 %dx%d 像素 / 逻辑 %dx%d（源帧 %dx%d，裁剪 %dx%d+%d+%d）\n",
                    win_w_, win_h_, out_w, out_h, crop.w, crop.h, frame_w, frame_h, crop.w, crop.h,
                    crop.x, crop.y);
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
        // 设了 logical size 之后渲染器坐标就是逻辑坐标，画满整个逻辑区域即可；
        // Retina 缩放和窗口拉伸后的等比留边由 SDL 负责。
        const SDL_Rect dst{0, 0, src_.w, src_.h};
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
    ///
    /// 尺寸必须问渲染器要**输出像素**，不能用窗口的逻辑点数：之前这里用的就是
    /// win_w_/win_h_，于是"内容只画满了左上四分之一"这种错自己完全看不出来——
    /// 读回来的恰好是自己画进去的那块，永远自洽。
    bool readback(const std::string &path) {
        int out_w = 0, out_h = 0;
        if (SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0 || out_w <= 0 ||
            out_h <= 0) {
            std::fprintf(stderr, "问绘制面尺寸失败: %s\n", SDL_GetError());
            return false;
        }
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32,
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
        const SDL_Rect full{0, 0, out_w, out_h};
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
        std::printf("已回读窗口内容 -> %s (%dx%d 像素)\n", path.c_str(), out_w, out_h);
        return true;
    }

    /// 泵一轮事件。触摸换算成"整块屏幕的 0..1 归一化坐标"再交出去——注入用的
    /// 就是这套坐标，与分辨率无关。
    ///
    /// 换算必须带上裁剪偏移：窗口看到的是显示区，而触摸面的 0..1 是相对**整块
    /// 屏幕**的。把窗口中间点成 0.5 只在"没裁剪"时才对，裁过之后要按裁剪框在
    /// 屏幕里的位置平移一遍，否则点哪儿都偏。
    bool pump(const std::function<void(double, double, bool)> &on_touch) {
        SDL_Event e;
        bool quit = false;
        // 一轮里可能堆了好几个 motion：只保留最后一个位置。鼠标 125Hz 往上时
        // 逐个发报告没有意义，设备侧要的是轨迹形状不是事件个数。
        bool pending_move = false;
        double px = 0, py = 0;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q) {
                        quit = true;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT && on_touch) {
                        dragging_ = true;
                        to_display(e.button.x, e.button.y, px, py);
                        on_touch(px, py, true);
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (dragging_ && on_touch) {
                        to_display(e.motion.x, e.motion.y, px, py);
                        pending_move = true;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT && on_touch) {
                        dragging_ = false;
                        if (pending_move) {
                            pending_move = false;
                            on_touch(px, py, true);
                        }
                        to_display(e.button.x, e.button.y, px, py);
                        on_touch(px, py, false);
                    }
                    break;
                default:
                    break;
            }
        }
        if (pending_move && on_touch) {
            pending_move = false;
            on_touch(px, py, true);
        }
        return quit;
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
    /// 窗口坐标 -> 整块屏幕的 0..1 归一化坐标。
    ///
    /// 必须经 SDL_RenderWindowToLogical：设了 logical size 之后，窗口和画面之间
    /// 可能有等比留边，自己按窗口尺寸除就会把点击算偏，而且窗口一拉偏得更明显。
    void to_display(int wx, int wy, double &fx, double &fy) const {
        float lx = 0, ly = 0;
        if (renderer_ != nullptr) {
            SDL_RenderWindowToLogical(renderer_, wx, wy, &lx, &ly);
        } else {
            lx = static_cast<float>(wx);
            ly = static_cast<float>(wy);
        }
        display_fraction_from_logical(lx, ly, src_, fx, fy);
    }

    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    Crop src_{};
    int win_w_ = 0, win_h_ = 0;
    /// 拿不到显示器边界时的兜底：按原始尺寸处理，等于不缩。
    static constexpr int win_w_fallback = 1 << 20;
    static constexpr int win_h_fallback = 1 << 20;
    bool dragging_ = false;
};

// ---------------------------------------------------------------- 帧的来源 ----

/// 一次 next 取到一帧已解码的图像。
///
/// 抽象成"取一帧"而不是"取一段字节"：字节层的事（分块喂、AU 边界、解码）现在
/// 两边各自解决了——实时走库里的 FramePump，文件走自己的解析器加解码器。主循环
/// 只关心"有没有新画面可以画"。
class FrameSource {
public:
    virtual ~FrameSource() = default;

    /// 返回 false 表示这次没取到（超时），调用方应该去泵一遍事件循环。
    /// `finished()` 为真才是真的结束。实现可以阻塞一小会儿再返回 false——
    /// 否则消费循环会在两帧之间空转，吃满一颗核还把窗口饿出事件事件。
    virtual bool next(scrctl::Frame &out, int timeout_ms) = 0;

    [[nodiscard]] virtual bool finished() const { return false; }
    /// 文件回放要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
    virtual void print_stats() const {}
};

/// 已录制的 Annex-B 文件。
///
/// 解析器是同步的——一次性 feed 整个文件会在任何一帧画出来之前就把上千帧全解进
/// 内存（每帧 11MB）。所以分块喂，并且解码结果攒在一个有上限的队列里。
class FileSource final : public FrameSource {
public:
    explicit FileSource(std::string path) : path_(std::move(path)) {}

    bool next(scrctl::Frame &out, int timeout_ms) override {
        (void)timeout_ms;  // 文件不会"等不到"，只会有"读完了"
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

    [[nodiscard]] bool finished() const override { return done_ && frames_.empty(); }
    [[nodiscard]] bool paces_itself() const override { return true; }

private:
    /// 读一块、喂给解析器、让回调往队列里放帧。队列满了就停手，下次再喂。
    bool pump_bytes(std::string &err) {
        if (!opened_ && !open_file(err)) {
            return false;
        }
        if (pos_ >= buffer_.size()) {
            // 收尾必须 flush：AU 的边界靠"下一个图像的起始 slice"判定，最后一个
            // AU 没有下一个，不 flush 就永远等不到它。
            if (!flushed_) {
                flushed_ = true;
                parser_->flush();
                done_ = true;
            }
            return false;
        }
        if (frames_.size() >= kMaxQueued) {
            return true;  // 背压：解码结果攒够了，先让调用方把它们画掉
        }
        const std::size_t n = std::min(kChunk, buffer_.size() - pos_);
        parser_->feed(buffer_.data() + pos_, n);
        pos_ += n;
        return true;
    }

    bool open_file(std::string &err) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            err = "打不开 " + path_;
            return false;
        }
        buffer_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        opened_ = true;
        std::printf("读入 %s (%zu 字节)\n", path_.c_str(), buffer_.size());

        decoder_ = scrctl::create_platform_decoder();
        std::printf("解码后端: %s\n", decoder_->backend_name());
        parser_ = std::make_unique<scrctl::AnnexBParser>(
            [this](std::vector<scrctl::Nal> &&au, bool) { this->on_au(std::move(au)); });
        return true;
    }

    void on_au(std::vector<scrctl::Nal> &&au) {
        if (!configured_) {
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
                !decoder_->configure(vps, sps, pps)) {
                return;
            }
            configured_ = true;
        }
        scrctl::Frame f;
        if (decoder_->decode(au, f) && f) {
            frames_.push_back(std::move(f));
        }
    }

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

/// 真机实时流。收包、拆 AU、解码、以及"画面坏了就重起会话"全在 FramePump 里，
/// 这里只管起流、取帧、以及把窗口的输入投回设备。
class LiveSource final : public FrameSource {
public:
    ~LiveSource() override;

    bool start(const std::string &serial, const std::string &record_path, std::string &err);

    bool next(scrctl::Frame &out, int timeout_ms) override {
        if (pump_ == nullptr) {
            return false;
        }
        const uint64_t got = pump_->newer(out, serial_, timeout_ms);
        if (got == 0) {
            return false;
        }
        serial_ = got;
        return true;
    }

    /// 把窗口里的一次触摸投到设备上。
    ///
    /// HID 服务**第一次用到时才连**：连接要一个来回，没必要把它算进起流路径；
    /// 而且设备不提供该服务（DDI 版本差异）时镜像应当照常工作，而不是整个退出。
    /// 失败过一次就不再重试，免得每帧都去撞一遍。
    bool control(double x, double y, bool down, std::string &err);

    /// 往设备敲一段 ASCII（复用触摸那条连接，键盘是同一个服务下的另一个面）。
    bool type_text(const std::string &text, int hold_ms, std::string &err);

    /// 按一个硬件按键（indigo 服务，惰性连）。按键的效果多半是瞬时的，所以
    /// 它得能和 `--verify` 组合使用：先按键，再等第 N 帧回读窗口内容。
    bool button(uint16_t usage_page, uint16_t usage_code, std::string &err);

    void print_stats() const override {
        if (pump_ == nullptr) {
            return;
        }
        const auto st = pump_->stats();
        std::printf("  流: 包 %llu 解码 %llu 未出帧 %llu 断流 %llu 次 重起 %llu 次\n",
                    static_cast<unsigned long long>(st.packets),
                    static_cast<unsigned long long>(st.decoded),
                    static_cast<unsigned long long>(st.no_output),
                    static_cast<unsigned long long>(st.gaps),
                    static_cast<unsigned long long>(st.restarts));
    }

private:
    std::unique_ptr<scrctl::remote::Device> device_;
    std::unique_ptr<scrctl::media::FramePump> pump_;
    std::unique_ptr<scrctl::hid::Service> hid_;
    std::unique_ptr<scrctl::hid::Buttons> buttons_;
    bool hid_unavailable_ = false;
    uint64_t serial_ = 0;
};

LiveSource::~LiveSource() = default;

bool LiveSource::start(const std::string &serial, const std::string &record_path,
                       std::string &err) {
    auto dev = scrctl::remote::Device::establish(serial, err);
    if (!dev) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));

    scrctl::media::FramePump::Options options;
    options.record_path = record_path;
    pump_ = scrctl::media::FramePump::start(*device_, options, err);
    if (pump_ == nullptr) {
        return false;
    }
    scrctl::Frame first;
    if (!pump_->latest(first, 5000)) {
        err = "5 秒内没解出第一帧";
        return false;
    }
    std::printf("流已建立：%s / iOS %s，收流端口=%u PT=%u，首帧 %ux%u\n",
                device_->property("ProductType").c_str(), device_->property("OSVersion").c_str(),
                pump_->receiver_port(), pump_->payload_type(), first.width, first.height);
    if (!record_path.empty()) {
        std::printf("录制到 %s\n", record_path.c_str());
    }
    return true;
}

bool LiveSource::control(double x, double y, bool down, std::string &err) {
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
        std::printf("控制已接通（触摸注入可用）\n");
    }
    return hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, down, err);
}

bool LiveSource::type_text(const std::string &text, int hold_ms, std::string &err) {
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
    }
    return hid_->type_text(text, hold_ms, err);
}

bool LiveSource::button(uint16_t usage_page, uint16_t usage_code, std::string &err) {
    if (buttons_ == nullptr) {
        buttons_ = scrctl::hid::Buttons::open(*device_, err);
        if (buttons_ == nullptr) {
            return false;
        }
    }
    return buttons_->press(usage_page, usage_code, 90, err);
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

    std::unique_ptr<FrameSource> source;
    LiveSource *live = nullptr;
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path);
    } else {
        auto made = std::make_unique<LiveSource>();
        std::string err;
        if (!made->start(o.serial, o.record, err)) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            return 1;
        }
        live = made.get();
        source = std::move(made);
    }
    const bool control_enabled = live != nullptr && !o.no_control;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    // 输入通路的无头自检：注入一条直线就退出。用直线而不是点一下，是因为
    // "画布被拖走一段"在截图上可判定，而一次点击在多数应用里没有可见后果。
    if (live != nullptr && !o.test_touch.empty()) {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (std::sscanf(o.test_touch.c_str(), "%f,%f,%f,%f", &x0, &y0, &x1, &y1) != 4) {
            std::fprintf(stderr, "--test-touch 格式应为 X0,Y0,X1,Y1，收到 %s\n",
                         o.test_touch.c_str());
            return 2;
        }
        std::string cerr;
        bool ok = true;
        for (int i = 0; i <= 20 && ok; ++i) {
            const double t = i / 20.0;
            ok = live->control(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, i < 20, cerr);
            if (i < 20) {
                SDL_Delay(12);
            }
        }
        std::printf("--test-touch (%.3f,%.3f)->(%.3f,%.3f): %s%s\n", x0, y0, x1, y1,
                    ok ? "已注入" : "失败", ok ? "" : cerr.c_str());
        return ok ? 0 : 1;
    }

    // 按键的判据要另想办法：音量 HUD 只显示一秒多，另起一次截图会话根本来不及。
    // 所以这里只负责"按下去"，看效果交给同一进程里已经在跑的镜像——
    // 配 --verify N 回读第 N 帧，HUD 就在那一帧里。
    if (live != nullptr && !o.test_button.empty()) {
        static const std::pair<const char *, uint16_t> kCodes[] = {
            {"home", scrctl::hid::button::kHome},   {"lock", scrctl::hid::button::kLock},
            {"volup", scrctl::hid::button::kVolumeUp},
            {"voldn", scrctl::hid::button::kVolumeDown}, {"mute", scrctl::hid::button::kMute},
        };
        uint16_t code = 0;
        for (const auto &e : kCodes) {
            if (o.test_button == e.first) {
                code = e.second;
                break;
            }
        }
        if (code == 0) {
            std::fprintf(stderr, "不认识按键 %s（可用：home/lock/volup/voldn/mute）\n",
                         o.test_button.c_str());
            return 2;
        }
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, code, berr)) {
            std::printf("--test-button %s: 已按下\n", o.test_button.c_str());
        } else {
            std::fprintf(stderr, "--test-button %s 失败: %s\n", o.test_button.c_str(),
                         berr.c_str());
            return 1;
        }
    }

    if (live != nullptr && !o.test_type.empty()) {
        std::string terr;
        if (live->type_text(o.test_type, 40, terr)) {
            std::printf("--test-type %s: 已注入\n", o.test_type.c_str());
        } else {
            std::fprintf(stderr, "--test-type 失败: %s\n", terr.c_str());
            return 1;
        }
    }

    int rendered = 0;
    std::unique_ptr<Presenter> presenter;
    const Uint64 start = SDL_GetTicks64();
    int last_reported = 0;
    bool quit = false;

    /// 窗口里的一次按下/移动/抬起 -> 设备上的接触/抬起。
    ///
    /// 注入失败只打一次：这是本地窗口在动鼠标，失败刷屏会把有用的帧率信息冲掉。
    std::string control_err;
    bool control_warned = false;
    auto on_touch = [&](double x, double y, bool down) {
        if (!control_enabled || live == nullptr) {
            return;
        }
        if (!live->control(x, y, down, control_err) && !control_warned) {
            control_warned = true;
            std::fprintf(stderr, "注入输入失败（已停止尝试）: %s\n", control_err.c_str());
        }
    };

    while (!quit) {
        scrctl::Frame f;
        // 50ms：再长一点，等帧期间窗口对关闭/移动的反应就开始发木。
        if (!source->next(f, 50)) {
            if (source->finished()) {
                std::printf("源已结束\n");
                break;
            }
            // 没帧可画也要让窗口活着——此刻基本都是在等下一帧到达，而等帧的时候
            // 不泵事件，窗口就是"未响应"。
            if (presenter != nullptr) {
                quit = presenter->pump(on_touch);
            }
            continue;
        }

        if (presenter == nullptr) {
            presenter = std::make_unique<Presenter>();
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 resolve_crop(o, f), o.scale, o.scale_given, o.title,
                                 o.verify_at > 0)) {
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
            std::printf("  渲染 %d 帧  %.1f fps\n", rendered, rendered / el);
            source->print_stats();
            last_reported = rendered;
        }
        if (o.exit_after > 0 && rendered >= o.exit_after) {
            std::printf("达到 --exit-after %d\n", o.exit_after);
            break;
        }
        quit = presenter->pump(on_touch);
    }

    std::printf("完成：渲染 %d 帧\n", rendered);
    presenter.reset();
    SDL_Quit();
    return 0;
}
