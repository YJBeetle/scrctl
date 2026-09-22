// scrctl M1：把已录制的 Annex-B HEVC 播放到原生窗口，验证解码+渲染链路。
//
// 解析器是同步的——一次性 feed 整个文件会在任何一帧被画出来之前就把上千帧
// 全解进内存（每帧 11MB）。所以这里分块喂，并用一个有上限的帧队列做背压。
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "bitstream/AnnexB.h"
#include "decode/Decoder.h"

namespace {

struct Options {
    std::string path;
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
        "用法: %s --play <file.hevc> [选项]\n"
        "\n"
        "  --play FILE          播放录制的 Annex-B HEVC\n"
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
    if (o.path.empty()) {
        std::fprintf(stderr, "需要 --play\n");
        return false;
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

    std::ifstream in(o.path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "打不开 %s\n", o.path.c_str());
        return 1;
    }
    std::vector<uint8_t> file{(std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>()};
    std::printf("读入 %s (%zu 字节)\n", o.path.c_str(), file.size());

    auto decoder = scrctl::create_platform_decoder();
    std::printf("解码后端: %s\n", decoder->backend_name());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    int rendered = 0, decoded = 0, failed = 0;
    bool configured = false;
    std::deque<scrctl::Frame> queue;
    // 只在队列空时才喂下一块，天然形成背压：最多一次块（约 3-4 帧）会先于
    // 渲染被解出来，不会把整个录制一次性读进内存。
    const size_t kChunk = 48 * 1024;

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
    size_t off = 0;
    const Uint64 start = SDL_GetTicks64();
    int last_reported = 0;

    bool quit = false;
    while (!quit) {
        while (queue.empty() && off < file.size()) {
            const size_t n = std::min(kChunk, file.size() - off);
            parser.feed(file.data() + off, n);
            off += n;
            if (queue.empty() && off >= file.size()) {
                parser.flush();
            }
        }
        if (queue.empty()) {
            std::printf("播放结束\n");
            break;
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

        // 按标称 60fps 节流，让录制以接近实时的速度播放。
        const Uint64 want_ms = static_cast<Uint64>(rendered) * 1000 / 60;
        const Uint64 now = SDL_GetTicks64() - start;
        if (want_ms > now) {
            SDL_Delay(static_cast<Uint32>(want_ms - now));
        }

        if (o.stats && rendered - last_reported >= 60) {
            const double el = (SDL_GetTicks64() - start) / 1000.0;
            std::printf("  渲染 %d 帧  %.1f fps  (解码 %d / 失败 %d)\n", rendered, rendered / el,
                        decoded, failed);
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
