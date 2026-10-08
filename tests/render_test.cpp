// 转屏渲染的离线自检：拿一张四角染色的假面板帧，在无头驱动下画一遍，读回来对角落。
//
// 为什么必须自己画一遍而不是只看数学：`SDL_RenderCopyEx` 的角度是顺时针还是逆时针，
// 是这条路径唯一一个"错了就整幅歪 180°"的外部约定，而真机上验它要开窗口（用户在场
// 时才合适），画面内容又一直在动、不好判定。dummy 驱动 + 软件渲染器 + 回读把这三件事
// 都绕开了：不需要显示器，像素是我们自己填的，判据是四个角的颜色落点。
//
// 这个测试确实抓到了两条只有画一遍才看得见的错：一是 dst 给成视口尺寸时 90/270 会
// 画成"中间一条、四角全黑"（SDL 是绕 dst 中心转、图像溢出 dst 的），二是 dummy 驱动下
// `SDL_SetWindowSize` 不带动绘制面尺寸。两种症状在真机窗口里都只是"画面不对"，
// 谁也推不回原因。
#include <SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "app/Presenter.h"
#include "app/RenderPanel.h"

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    } else {
        std::printf("  PASS %s\n", what);
    }
}

/// 面板尺寸与角块。取 40x60 是为了让"角块中心"离边缘有 6px，采样时不会被
/// 纹理边缘的插值尾巴影响；取整宽高是为了 1:1 画进视口、完全不经过缩放滤波。
constexpr int kPanelW = 40;
constexpr int kPanelH = 60;
constexpr int kBlock = 12;

struct Rgb {
    Uint8 r, g, b;
};

/// 四个角块的颜色。刻意选得互相差很远，任何一档转错都会立刻对不上。
constexpr Rgb kTopLeft {220, 20, 20};      // 红
constexpr Rgb kTopRight {20, 200, 20};     // 绿
constexpr Rgb kBottomRight {30, 60, 230};  // 蓝
constexpr Rgb kBottomLeft {240, 240, 240}; // 白
constexpr Rgb kBackground {10, 10, 10};

Uint32 pack(Rgb c) {
    return (Uint32(255) << 24) | (Uint32(c.r) << 16) | (Uint32(c.g) << 8) | c.b;
}

/// 面板上的角块中心（与下面算视口落点时用同一套坐标）。
struct Point {
    int x, y;
};

Point panel_corner(int which) {
    switch (which) {
        case 0: return {kBlock / 2, kBlock / 2};
        case 1: return {kPanelW - kBlock / 2, kBlock / 2};
        case 2: return {kPanelW - kBlock / 2, kPanelH - kBlock / 2};
        default: return {kBlock / 2, kPanelH - kBlock / 2};
    }
}

/// "视口 = 面板顺时针转 degrees"这个约定下，面板上的点落在视口的哪里。
///
/// 这里独立地按定义算一遍（先归一化，再按顺时针旋转的坐标变换），为的是让判据
/// 不是从被测代码里抄来的：被测的是 SDL 的角度方向与 dst 尺寸，判据必须来自
/// 我们对"顺时针"的理解。
Point viewport_corner(int which, int degrees) {
    const Point p = panel_corner(which);
    const double u = static_cast<double>(p.x) / kPanelW;  // 面板横向 0..1
    const double v = static_cast<double>(p.y) / kPanelH;  // 面板纵向 0..1，向下
    double tu = u, tv = v;
    switch (degrees) {
        case 90: tu = 1.0 - v; tv = u; break;
        case 180: tu = 1.0 - u; tv = 1.0 - v; break;
        case 270: tu = v; tv = 1.0 - u; break;
        default: break;
    }
    int vw = kPanelW, vh = kPanelH;
    if (degrees == 90 || degrees == 270) {
        vw = kPanelH;
        vh = kPanelW;
    }
    return {static_cast<int>(tu * vw + 0.5), static_cast<int>(tv * vh + 0.5)};
}

Rgb at(const Uint32 *pixels, int w, int x, int y) {
    const Uint32 p = pixels[y * w + x];
    return {static_cast<Uint8>((p >> 16) & 0xFF), static_cast<Uint8>((p >> 8) & 0xFF),
            static_cast<Uint8>(p & 0xFF)};
}

bool close(Rgb a, Rgb b) {
    const auto d = [](Uint8 x, Uint8 y) { return x > y ? x - y : y - x; };
    return d(a.r, b.r) < 24 && d(a.g, b.g) < 24 && d(a.b, b.b) < 24;
}

/// 一档一次性的窗口 + 渲染器。
///
/// 为什么每档新建、又为什么上 RAII：想改成 `SDL_SetWindowSize` 复用，实测 dummy 驱动下
/// 绘制面并不跟着变（视口 40x60 仍报 60x60），于是回读读到的是留边后的局部，四个角
/// 全对不上——症状和"旋转方向搞反"一模一样，很难查。而手写 destroy 的出口有五个，
/// 漏掉一个就是下一档读到上一档的残留。
struct Canvas {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    ~Canvas() {
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
    Canvas() = default;
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    bool open(int w, int h) {
        window = SDL_CreateWindow("render_test", 0, 0, w, h, SDL_WINDOW_HIDDEN);
        if (window == nullptr) {
            std::printf("  FAIL 建窗口: %s\n", SDL_GetError());
            return false;
        }
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == nullptr) {
            std::printf("  FAIL 建软件渲染器: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }
};

struct Texture {
    SDL_Texture *tex = nullptr;
    ~Texture() {
        if (tex != nullptr) {
            SDL_DestroyTexture(tex);
        }
    }
    Texture() = default;
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
};

/// 画一次并回读。返回 false 表示这一档根本没画成（建纹理/回读失败）。
bool render_once(int degrees, std::vector<Uint32> &out, int &ow, int &oh) {
    scrctl::app::Crop crop {};
    crop.x = 0;
    crop.y = 0;
    crop.w = kPanelW;
    crop.h = kPanelH;
    crop.display_w = kPanelW;
    crop.display_h = kPanelH;
    int vw = 0, vh = 0;
    scrctl::app::viewport_size(crop, degrees, vw, vh);
    ow = vw;
    oh = vh;

    Canvas canvas;
    if (!canvas.open(vw, vh)) {
        return false;
    }
    // 窗口点数必须与 logical size 一致。不一致时 SDL 会按等比留边把画面居中，
    // 于是"读回 0..vw x 0..vh"读到的是一块带偏移的局部，判据就全错了——所以在这里
    // 先把它断掉，而不是等到角落颜色对不上才发现。
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(canvas.renderer, &out_w, &out_h);
    if (out_w != vw || out_h != vh) {
        std::printf("  FAIL 绘制面 %dx%d 与视口 %dx%d 不符，回读会读到留边后的局部\n", out_w, out_h,
                    vw, vh);
        return false;
    }
    if (SDL_RenderSetLogicalSize(canvas.renderer, vw, vh) != 0) {
        std::printf("  FAIL 设 logical size: %s\n", SDL_GetError());
        return false;
    }
    Texture holder;
    // STREAMING 而不是 STATIC：软件渲染器只允许锁 STREAMING 纹理，
    // 用 STATIC 会在 SDL_LockTexture 里报 "texture must be streaming"。
    holder.tex = SDL_CreateTexture(canvas.renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, kPanelW, kPanelH);
    if (holder.tex == nullptr) {
        std::printf("  FAIL 建纹理: %s\n", SDL_GetError());
        return false;
    }
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(holder.tex, nullptr, &pixels, &pitch) != 0) {
        std::printf("  FAIL 锁纹理: %s\n", SDL_GetError());
        return false;
    }
    for (int y = 0; y < kPanelH; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(
            static_cast<Uint8 *>(pixels) + static_cast<std::size_t>(y) * pitch);
        for (int x = 0; x < kPanelW; ++x) {
            Rgb c = kBackground;
            if (x < kBlock && y < kBlock) {
                c = kTopLeft;
            } else if (x >= kPanelW - kBlock && y < kBlock) {
                c = kTopRight;
            } else if (x >= kPanelW - kBlock && y >= kPanelH - kBlock) {
                c = kBottomRight;
            } else if (x < kBlock && y >= kPanelH - kBlock) {
                c = kBottomLeft;
            }
            row[x] = pack(c);
        }
    }
    SDL_UnlockTexture(holder.tex);

    SDL_SetRenderDrawColor(canvas.renderer, 0, 0, 0, 255);
    SDL_RenderClear(canvas.renderer);
    scrctl::app::draw_rotated(canvas.renderer, holder.tex, crop, degrees);

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, vw, vh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s == nullptr) {
        std::printf("  FAIL 建回读面: %s\n", SDL_GetError());
        return false;
    }
    const SDL_Rect full {0, 0, vw, vh};
    const int rc = SDL_RenderReadPixels(canvas.renderer, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels,
                                        s->pitch);
    if (rc != 0) {
        std::printf("  FAIL 回读: %s\n", SDL_GetError());
        SDL_FreeSurface(s);
        return false;
    }
    out.assign(static_cast<std::size_t>(vw) * vh, 0);
    // 逐行搬：surface 的 pitch 可能比 vw*4 大，整块 copy 会把行间距当成像素读进来。
    for (int y = 0; y < vh; ++y) {
        const auto *src_row = reinterpret_cast<const Uint32 *>(
            static_cast<const Uint8 *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch);
        for (int x = 0; x < vw; ++x) {
            out[static_cast<std::size_t>(y) * vw + x] = src_row[x];
        }
    }
    SDL_FreeSurface(s);
    return true;
}

/// 等比留边那两条边的颜色，以及"回读到底覆盖了整块输出没有"。
///
/// 两条各自独立、都栽过的判据：
///   * `SDL_RenderClear` 清的是**当前视口**，而设了 logical size 之后视口就是等比
///     缩放后那块内容区——两条边在视口外面，清不到。所以清之前要把 logical size
///     摘掉、清完再挂回来。
///   * `SDL_RenderReadPixels` 的矩形在挂着 logical size 时是按**逻辑**坐标解释的，
///     传整块输出的尺寸读回来的是一个偏移过的局部（内容区之外的坐标直接失败）。
///     读之前同样要摘掉。
/// 这两条都要用"窗口比例 != 画面比例"的场景才看得见，而默认窗口是按画面比例算的，
/// 所以产品自检里必须显式造一个不等的。
bool letterbox_and_readback() {
    static constexpr int kOutW = 800, kOutH = 400;
    SDL_Window *window = SDL_CreateWindow("letterbox", 0, 0, kOutW, kOutH, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::printf("  建窗口失败: %s\n", SDL_GetError());
        return false;
    }
    SDL_Renderer *r = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         1136, 2464);
    std::vector<Uint32> pixels(1136u * 2464u, 0xFFFFFFFFu);  // 整帧纯白
    SDL_UpdateTexture(tex, nullptr, pixels.data(), 1136 * 4);

    SDL_RenderSetLogicalSize(r, 1125, 2436);
    SDL_RenderSetLogicalSize(r, 0, 0);
    SDL_SetRenderDrawColor(r, 18, 52, 86, 255);  // #123456
    SDL_RenderClear(r);
    SDL_RenderSetLogicalSize(r, 1125, 2436);
    SDL_RenderCopy(r, tex, nullptr, nullptr);
    SDL_RenderSetLogicalSize(r, 0, 0);

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, kOutW, kOutH, 32, SDL_PIXELFORMAT_ARGB8888);
    const SDL_Rect full {0, 0, kOutW, kOutH};
    const bool read_ok =
        SDL_RenderReadPixels(r, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0;
    bool pass = read_ok;
    if (read_ok) {
        SDL_LockSurface(s);
        auto at = [&](int x, int y) {
            const auto *p = static_cast<const Uint8 *>(s->pixels) + y * s->pitch + x * 4;
            return Rgb {p[2], p[1], p[0]};
        };
        // 1125x2436 放进 800x400：缩放 0.1642，内容宽 185，居中 => 左边 307..308 是边
        pass = close(at(0, kOutH / 2), Rgb {18, 52, 86}) &&
               close(at(300, kOutH / 2), Rgb {18, 52, 86}) &&
               close(at(kOutW - 1, kOutH / 2), Rgb {18, 52, 86}) &&
               close(at(kOutW / 2, kOutH / 2), Rgb {255, 255, 255}) &&
               close(at(320, kOutH / 2), Rgb {255, 255, 255}) &&
               close(at(480, kOutH / 2), Rgb {255, 255, 255});
        if (!pass) {
            std::printf("  边上/内容颜色不对：边=(%u,%u,%u) 内容=(%u,%u,%u)\n",
                        at(0, 200).r, at(0, 200).g, at(0, 200).b, at(400, 200).r, at(400, 200).g,
                        at(400, 200).b);
        }
        SDL_UnlockSurface(s);
    } else {
        std::printf("  回读整块输出失败: %s\n", SDL_GetError());
    }
    SDL_FreeSurface(s);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(window);
    return pass;
}

struct ReadbackDirectory {
    std::filesystem::path path;

    bool open() {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) return false;
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto candidate = base / ("scrctl-render-test-" + std::to_string(id));
        if (!std::filesystem::create_directory(candidate, error)) return false;
        path = candidate;
        return true;
    }

    ~ReadbackDirectory() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

using Palette = std::array<Rgb, 4>;

scrctl::Frame colored_frame(int width, int height, const scrctl::app::Crop &crop,
                            const Palette &colors, int row_padding) {
    scrctl::Frame frame;
    frame.width = width;
    frame.height = height;
    frame.row_pitch = width * 4 + row_padding;
    // 最后一行不需要尾部 padding；这个合法边界也必须可以上传。
    frame.pixels.resize(static_cast<std::size_t>(height - 1) * frame.row_pitch + width * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Rgb color{255, 0, 255};  // 编码填充区，不能出现在裁剪后的画面内。
            if (x < crop.w && y < crop.h) {
                color = kBackground;
                const bool left = x < crop.w / 4;
                const bool right = x >= crop.w - crop.w / 4;
                const bool top = y < crop.h / 4;
                const bool bottom = y >= crop.h - crop.h / 4;
                if (left && top) color = colors[0];
                else if (right && top) color = colors[1];
                else if (right && bottom) color = colors[2];
                else if (left && bottom) color = colors[3];
            }
            const auto offset = static_cast<std::size_t>(y) * frame.row_pitch + x * 4;
            frame.pixels[offset] = color.b;
            frame.pixels[offset + 1] = color.g;
            frame.pixels[offset + 2] = color.r;
            frame.pixels[offset + 3] = 255;
        }
    }
    return frame;
}

void check_presenter_readback(const std::string &path, const Palette &colors,
                               int width, int height, const char *stage) {
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    Surface loaded(SDL_LoadBMP(path.c_str()), SDL_FreeSurface);
    check(loaded != nullptr, "Presenter 回读文件可作为 BMP 加载");
    if (!loaded) return;
    check(loaded->w == width && loaded->h == height, "尺寸切换保留窗口的输出尺寸");
    if (loaded->w != width || loaded->h != height) return;
    Surface pixels(SDL_ConvertSurfaceFormat(loaded.get(), SDL_PIXELFORMAT_ARGB8888, 0),
                   SDL_FreeSurface);
    check(pixels != nullptr, "Presenter 回读像素可转换为 ARGB8888");
    if (!pixels) return;
    if (SDL_LockSurface(pixels.get()) != 0) {
        check(false, "Presenter 回读像素可锁定");
        return;
    }
    const Point samples[4] = {{width / 8, height / 8}, {width - width / 8, height / 8},
                              {width - width / 8, height - height / 8},
                              {width / 8, height - height / 8}};
    for (int corner = 0; corner < 4; ++corner) {
        const auto *address = static_cast<const Uint8 *>(pixels->pixels) +
            samples[corner].y * pixels->pitch + samples[corner].x * 4;
        Uint32 packed = 0;
        std::memcpy(&packed, address, sizeof packed);
        const Rgb actual{static_cast<Uint8>((packed >> 16) & 255),
                         static_cast<Uint8>((packed >> 8) & 255),
                         static_cast<Uint8>(packed & 255)};
        char note[128];
        std::snprintf(note, sizeof note, "%s 第 %d 个角显示当前帧颜色", stage, corner + 1);
        check(close(actual, colors[corner]), note);
        if (!close(actual, colors[corner])) {
            std::printf("     实际 rgb(%u,%u,%u)\n", actual.r, actual.g, actual.b);
        }
    }
    SDL_UnlockSurface(pixels.get());
}

void presenter_source_size_changes() {
    ReadbackDirectory output;
    check(output.open(), "创建 Presenter 回读临时目录");
    if (output.path.empty()) return;

    // 视频包含编码填充，截图较小；视口比例相同，逻辑尺寸与纹理尺寸都需要更新。
    const scrctl::app::Crop video_crop{0, 0, 90, 135, 90, 135};
    const scrctl::app::Crop screenshot_crop{0, 0, 60, 90, 60, 90};
    const Palette first_colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    const Palette screenshot_colors{kBottomLeft, kBottomRight, kTopRight, kTopLeft};
    const Palette restored_colors{kBottomRight, kTopLeft, kBottomLeft, kTopRight};
    const auto video = colored_frame(96, 144, video_crop, first_colors, 16);
    const auto screenshot = colored_frame(60, 90, screenshot_crop, screenshot_colors, 0);
    const auto restored = colored_frame(96, 144, video_crop, restored_colors, 32);

    scrctl::app::Presenter presenter;
    scrctl::app::WindowSpec spec;
    spec.title = "Presenter offline regression";
    spec.want_w = 90;
    spec.want_h = 135;
    spec.want_readback = true;
    const bool opened = presenter.open(video.width, video.height, video_crop, 0, 1, false, spec);
    check(opened, "使用实际 Presenter 创建软件渲染器");
    if (!opened) return;

    const auto draw_and_check = [&](const scrctl::Frame &frame, const scrctl::app::Crop &crop,
                                    const Palette &colors, const char *name) {
        const auto path = (output.path / (std::string(name) + ".bmp")).string();
        check(presenter.draw(frame, crop, path.c_str()), name);
        check_presenter_readback(path, colors, spec.want_w, spec.want_h, name);
    };
    draw_and_check(video, video_crop, first_colors, "video");
    draw_and_check(screenshot, screenshot_crop, screenshot_colors, "screenshot");
    draw_and_check(restored, video_crop, restored_colors, "video-restored");

    auto invalid = restored;
    invalid.pixels.pop_back();
    check(!presenter.draw(invalid, video_crop), "拒绝缺少最后一个像素字节的缓冲");
    invalid = restored;
    invalid.row_pitch = invalid.width * 4 - 1;
    check(!presenter.draw(invalid, video_crop), "拒绝小于整行像素宽度的 pitch");
    invalid.row_pitch = uint32_t(std::numeric_limits<int>::max()) + 1;
    check(!presenter.draw(invalid, video_crop), "拒绝 SDL 无法表示的 pitch");
    auto invalid_crop = video_crop;
    invalid_crop.w = restored.width + 1;
    check(!presenter.draw(restored, invalid_crop), "拒绝超出像素缓冲范围的裁剪");
    invalid_crop = video_crop;
    invalid_crop.x = -1;
    check(!presenter.draw(restored, invalid_crop), "拒绝负数裁剪起点");
    const auto missing_parent = (output.path / "missing" / "frame.bmp").string();
    check(!presenter.draw(restored, video_crop, missing_parent.c_str()),
          "回读路径的父目录不存在时绘制返回失败");
    check(!std::filesystem::exists(missing_parent), "失败的回读没有生成输出文件");
    draw_and_check(video, video_crop, first_colors, "video-after-errors");
}

void presenter_releases_touch_when_geometry_changes() {
    const scrctl::app::Crop crop{10, 20, 40, 60, 80, 120};
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    const auto frame = colored_frame(80, 120, crop, colors, 0);
    scrctl::app::Presenter presenter;
    scrctl::app::WindowSpec spec;
    spec.want_w = crop.w;
    spec.want_h = crop.h;
    spec.want_readback = true;
    const bool opened = presenter.open(frame.width, frame.height, crop, 0, 1, false, spec);
    check(opened, "为 SDL 输入事件回归创建实际 Presenter");
    if (!opened) return;
    check(presenter.draw(frame, crop), "输入事件前绘制有效几何帧");
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    struct Touch {
        double x, y;
        bool down;
    };
    std::vector<Touch> callbacks;
    const auto on_touch = [&](double x, double y, bool down) {
        callbacks.push_back({x, y, down});
        std::printf("     callback #%zu: native(%.6f, %.6f) %s\n", callbacks.size(), x, y,
                    down ? "down" : "up");
    };
    const auto push_mouse = [&](Uint32 type, int x, int y) {
        SDL_Event event{};
        event.type = type;
        // 不指定 windowID，直接注入 Presenter 使用的 logical size 坐标；
        // 避免 SDL 的窗口事件 watch 再把坐标按窗口点数缩放一次。
        if (type == SDL_MOUSEMOTION) {
            event.motion.state = SDL_BUTTON_LMASK;
            event.motion.x = x;
            event.motion.y = y;
        } else {
            event.button.button = SDL_BUTTON_LEFT;
            event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.button.x = x;
            event.button.y = y;
        }
        check(SDL_PushEvent(&event) == 1, "将鼠标事件送入实际 SDL 队列");
    };
    const auto check_touch = [&](std::size_t index, bool down, double x, double y,
                                  const char *message) {
        check(index < callbacks.size() && callbacks[index].down == down &&
                  std::fabs(callbacks[index].x - x) < 1e-9 &&
                  std::fabs(callbacks[index].y - y) < 1e-9,
              message);
    };

    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    push_mouse(SDL_MOUSEMOTION, 20, 30);
    check(!presenter.pump(on_touch), "有效几何下处理按下和移动事件");
    check(callbacks.size() == 2, "按下与最后一次移动分别交付设备回调");
    check_touch(0, true, 18.0 / 80, 32.0 / 120, "按下使用裁剪偏移后的面板坐标");
    check_touch(1, true, 30.0 / 80, 50.0 / 120, "记录最后一次有效移动的设备坐标");

    auto unknown = crop;
    unknown.input_valid = false;
    check(presenter.draw(frame, unknown), "未知方向仍可绘制画面");
    push_mouse(SDL_MOUSEBUTTONDOWN, 4, 54);
    push_mouse(SDL_MOUSEMOTION, 16, 12);
    push_mouse(SDL_MOUSEBUTTONUP, 4, 54);
    check(!presenter.pump(on_touch), "未知几何下处理队列并释放此前触摸");
    check(callbacks.size() == 3, "未知几何只产生一次抬起，拒绝新的按下和移动");
    check_touch(2, false, 30.0 / 80, 50.0 / 120, "抬起沿用旧的最后有效设备点");
    presenter.pump(on_touch);
    presenter.release_touch(on_touch);
    check(callbacks.size() == 3, "重复 pump 或 release_touch 不重复抬起");

    check(presenter.draw(frame, crop), "恢复已知方向的几何帧");
    push_mouse(SDL_MOUSEBUTTONDOWN, 4, 48);
    presenter.pump(on_touch);
    check(callbacks.size() == 4, "几何恢复后可重新按下");
    check_touch(3, true, 14.0 / 80, 68.0 / 120, "恢复后的按下仍使用当前面板坐标");

    auto rotated = crop;
    rotated.pixel_degrees = 180;
    check(presenter.draw(frame, rotated), "源像素转向改变，窗口渲染角仍为零");
    presenter.pump(on_touch);
    check(callbacks.size() == 5, "源像素方向改变也释放已有触摸");
    check_touch(4, false, 14.0 / 80, 68.0 / 120, "转向后的抬起不按新坐标轴重算旧触点");
    presenter.release_touch(on_touch);
    presenter.release_touch(on_touch);
    check(callbacks.size() == 5, "转向释放后的 release_touch 保持幂等");

    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    presenter.pump(on_touch);
    check_touch(5, true, 1.0 - 18.0 / 80, 1.0 - 32.0 / 120,
                "新按下使用源像素已旋转 180 度的逆变换");
    auto resized_panel = rotated;
    resized_panel.display_w = 160;
    resized_panel.display_h = 240;
    check(presenter.draw(frame, resized_panel), "面板尺寸改变但源像素与窗口尺寸保持不变");
    presenter.pump(on_touch);
    check(callbacks.size() == 7, "面板尺寸改变也释放已有触摸");
    check_touch(6, false, 1.0 - 18.0 / 80, 1.0 - 32.0 / 120,
                "尺寸改变后的抬起不使用新的归一化分母");
    check(presenter.draw(frame, rotated), "恢复旋转后的面板尺寸");
    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    presenter.pump(on_touch);
    check_touch(7, true, 1.0 - 18.0 / 80, 1.0 - 32.0 / 120,
                "尺寸恢复后可再次按下");
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    check(SDL_PushEvent(&quit) == 1, "将退出事件送入实际 SDL 队列");
    check(presenter.pump(on_touch), "退出事件返回结束请求");
    check(callbacks.size() == 9, "退出时释放一次仍按下的设备触点");
    check_touch(8, false, 1.0 - 18.0 / 80, 1.0 - 32.0 / 120,
                "退出抬起使用最后有效的旋转后设备坐标");
    presenter.release_touch(on_touch);
    check(callbacks.size() == 9, "退出后的重复释放不新增回调");
}

void presenter_shortcuts_preserve_normal_input() {
    const scrctl::app::Crop crop{0, 0, 64, 96, 64, 96};
    scrctl::app::WindowSpec spec;
    spec.title = "Presenter shortcut regression";
    spec.want_w = crop.w;
    spec.want_h = crop.h;
    spec.want_readback = true;
    scrctl::app::Presenter presenter;
    const bool default_opened = presenter.open(64, 96, crop, 0, 1, false, spec);
    check(default_opened, "创建快捷键事件回归窗口");
    if (!default_opened) return;
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    int presses = 0, releases = 0;
    const auto on_touch = [&](double, double, bool down) { down ? ++presses : ++releases; };
    const auto key = [&](SDL_Keycode symbol, Uint16 mods = KMOD_NONE, Uint8 repeat = 0) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = symbol;
        event.key.keysym.mod = mods;
        event.key.repeat = repeat;
        check(SDL_PushEvent(&event) == 1, "将键盘事件送入实际 SDL 队列");
        return presenter.pump(on_touch);
    };
    SDL_Event down{};
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = 16;
    down.button.y = 24;
    check(SDL_PushEvent(&down) == 1 && !presenter.pump(on_touch), "快捷键测试前按下设备触点");
    check(!key(SDLK_q) && !key(SDLK_q, KMOD_LSHIFT) && !key(SDLK_ESCAPE),
          "普通 q、Q 和 Esc 不退出窗口");
    check(!key(SDLK_q, KMOD_LCTRL) && !key(SDLK_q, KMOD_RGUI) &&
              !key(SDLK_q, KMOD_RALT | KMOD_LCTRL),
          "默认不截获 Ctrl+Q、右 Super+Q 或 AltGr+Q");
    check(presses == 1 && releases == 0, "普通键不会结束正在进行的设备拖动");
    check(key(SDLK_q, KMOD_LALT), "默认左 Alt+Q 退出");
    check(presses == 1 && releases == 1, "快捷键退出释放一次设备触点");
    check(key(SDLK_q, KMOD_LGUI), "默认左 Super+Q 退出");
    check(!key(SDLK_q, KMOD_LALT, 1), "忽略退出快捷键的重复按键事件");
    check(!presenter.is_fullscreen(), "窗口最初为普通模式");
    key(SDLK_F11);
    check(presenter.is_fullscreen(), "无修饰 F11 进入全屏");
    key(SDLK_F11, KMOD_NONE, 1);
    key(SDLK_F11, KMOD_LCTRL);
    key(SDLK_f, KMOD_LALT | KMOD_LSHIFT);
    check(presenter.is_fullscreen(), "重复 F11、Ctrl+F11 和 MOD+Shift+F 不切换全屏");
    key(SDLK_f, KMOD_LALT);
    check(!presenter.is_fullscreen(), "MOD+F 恢复普通窗口");
    key(SDLK_f, KMOD_LGUI);
    check(presenter.is_fullscreen(), "左 Super+F 进入全屏");
    key(SDLK_F11);
    check(!presenter.is_fullscreen(), "F11 可以退出全屏");

    spec.shortcut_mods = KMOD_RCTRL;
    spec.fullscreen = true;
    scrctl::app::Presenter configured;
    const bool opened = configured.open(64, 96, crop, 0, 1, false, spec);
    check(opened, "使用自定义修饰键创建全屏窗口");
    if (!opened) return;
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    const auto configured_key = [&](Uint16 mods, SDL_Keycode symbol) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = symbol;
        event.key.keysym.mod = mods;
        check(SDL_PushEvent(&event) == 1, "送入自定义快捷键事件");
        return configured.pump({});
    };
    check(!configured_key(KMOD_LALT, SDLK_q) && configured_key(KMOD_RCTRL, SDLK_q),
          "自定义修饰键替换默认退出组合");
    configured_key(KMOD_RCTRL, SDLK_f);
    check(!configured.is_fullscreen(), "从启动全屏模式退出后仍可切换窗口模式");
}

}  // namespace

int main() {
    // 必须在 SDL_Init 之前设：dummy 驱动不需要显示器，CI 与 SSH 会话里也能跑。
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }
    static constexpr const char *kNames[4] = {"面板左上(红)", "面板右上(绿)", "面板右下(蓝)",
                                              "面板左下(白)"};
    static constexpr Rgb kColors[4] = {kTopLeft, kTopRight, kBottomRight, kBottomLeft};

    for (const int degrees : {0, 90, 180, 270}) {
        std::printf("== 顺时针转正 %d° ==\n", degrees);
        std::vector<Uint32> px;
        int ow = 0, oh = 0;
        const int before = failures;
        if (!render_once(degrees, px, ow, oh)) {
            ++failures;
            continue;
        }
        // 视口尺寸本身是一条判据：转 90/270 必须宽高对调，否则窗口比例就是错的。
        const int want_w = (degrees == 90 || degrees == 270) ? kPanelH : kPanelW;
        const int want_h = (degrees == 90 || degrees == 270) ? kPanelW : kPanelH;
        check(ow == want_w && oh == want_h, "视口尺寸按旋转对调");
        for (int i = 0; i < 4; ++i) {
            const Point want = viewport_corner(i, degrees);
            const Rgb got = at(px.data(), ow, want.x, want.y);
            char note[128];
            std::snprintf(note, sizeof note, "%s 落在视口 (%d,%d) 且颜色对得上", kNames[i], want.x,
                          want.y);
            const bool ok = close(got, kColors[i]);
            check(ok, note);
            if (!ok) {
                std::printf("     实际 rgb(%u,%u,%u)\n", got.r, got.g, got.b);
            }
        }
        if (failures != before) {
            std::printf("  ^ %d° 这一档错了：SDL 的角度方向或 dst 尺寸与约定不符\n", degrees);
        }
    }

    check(letterbox_and_readback(), "等比留边的边上涂的是 --background-color，且回读覆盖整块输出");
    presenter_source_size_changes();
    presenter_releases_touch_when_geometry_changes();
    presenter_shortcuts_preserve_normal_input();

    SDL_Quit();
    if (failures != 0) {
        std::printf("render_test: %d 项失败\n", failures);
        return 1;
    }
    std::printf("render_test: 全部通过\n");
    return 0;
}
