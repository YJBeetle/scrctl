// SDL 显示与输入的离线回归。dummy 驱动和软件渲染器不依赖显示器或设备，
// 用固定源像素检查实际输出，而不只验证坐标公式。
// 四角颜色验证旋转/翻转顺序，每像素不同的裁剪帧检测半像素中心产生的偏移；
// Presenter 的 SDL 指针事件回调则检查实际显示方向与设备坐标是否一致。
// 另保留留边、回读、来源尺寸切换和触点释放回归。
#include <SDL.h>

#include <algorithm>
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

Uint32 window_id_from_events(const char *title) {
    Uint32 window_id = 0;
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type != SDL_WINDOWEVENT) continue;
        SDL_Window *window = SDL_GetWindowFromID(event.window.windowID);
        if (window && std::strcmp(SDL_GetWindowTitle(window), title) == 0) {
            window_id = SDL_GetWindowID(window);
        }
    }
    check(window_id != 0, "从真实 SDL 窗口事件取得被测 Presenter 的 windowID");
    return window_id;
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
Point viewport_corner(int which, int degrees, bool horizontal_flip = false) {
    const Point p = panel_corner(which);
    const double original_u = static_cast<double>(p.x) / kPanelW;
    const double u = horizontal_flip ? 1.0 - original_u : original_u;
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
bool render_once(int degrees, std::vector<Uint32> &out, int &ow, int &oh,
                 bool horizontal_flip = false) {
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
            Rgb c{255, 0, 255}; // 裁剪区域外用不同颜色，检测误翻整张纹理或越界显示。
            const int cx = x - crop.x, cy = y - crop.y;
            if (cx >= 0 && cy >= 0 && cx < crop.w && cy < crop.h) {
                c = kBackground;
                const int block = std::min({kBlock, crop.w / 4, crop.h / 4});
                if (cx < block && cy < block) c = kTopLeft;
                else if (cx >= crop.w - block && cy < block) c = kTopRight;
                else if (cx >= crop.w - block && cy >= crop.h - block) c = kBottomRight;
                else if (cx < block && cy >= crop.h - block) c = kBottomLeft;
            }
            row[x] = pack(c);
        }
    }
    SDL_UnlockTexture(holder.tex);

    SDL_SetRenderDrawColor(canvas.renderer, 0, 0, 0, 255);
    SDL_RenderClear(canvas.renderer);
    if (!scrctl::app::draw_rotated(canvas.renderer, holder.tex, crop, degrees, horizontal_flip))
        return false;

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

/// 每像素不同的非正方形裁剪验证中心精度，四角大色块无法发现一像素偏移。
/// 同时覆盖宽高同奇偶和不同奇偶，判据直接来自离散像素的翻转与旋转定义。
void cropped_flip_pixels() {
    for (const auto size : {Point{24, 36}, Point{25, 36}, Point{24, 35}, Point{25, 35}}) {
        const scrctl::app::Crop crop{6, 8, size.x, size.y, kPanelW, kPanelH};
        std::vector<Uint32> source(kPanelW * kPanelH);
        for (int y = 0; y < kPanelH; ++y)
            for (int x = 0; x < kPanelW; ++x)
                source[y * kPanelW + x] = pack(Rgb{static_cast<Uint8>(x * 5),
                                                   static_cast<Uint8>(y * 3),
                                                   static_cast<Uint8>(x + y)});
        for (const int degrees : {0, 90, 180, 270}) for (const bool flip : {false, true}) {
            int vw = 0, vh = 0;
            scrctl::app::viewport_size(crop, degrees, vw, vh);
            Canvas canvas;
            check(canvas.open(vw, vh), "为奇偶裁剪像素回归创建软件渲染器");
            if (!canvas.renderer) continue;
            Texture texture;
            texture.tex = SDL_CreateTexture(canvas.renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, kPanelW, kPanelH);
            check(texture.tex != nullptr, "创建裁剪像素纹理");
            if (!texture.tex) continue;
            SDL_SetTextureScaleMode(texture.tex, SDL_ScaleModeNearest);
            check(SDL_UpdateTexture(texture.tex, nullptr, source.data(), kPanelW * 4) == 0,
                  "上传每像素不同的完整源帧");
            SDL_SetRenderDrawColor(canvas.renderer, 0, 0, 0, 255);
            SDL_RenderClear(canvas.renderer);
            check(scrctl::app::draw_rotated(canvas.renderer, texture.tex, crop, degrees, flip),
                  "用正式绘制入口渲染裁剪及翻转");
            std::vector<Uint32> actual(static_cast<std::size_t>(vw) * vh);
            const bool read = SDL_RenderReadPixels(canvas.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                                    actual.data(), vw * 4) == 0;
            bool equal = read;
            for (int y = 0; y < crop.h && equal; ++y) for (int x = 0; x < crop.w; ++x) {
                const int hx = flip ? crop.w - 1 - x : x;
                int dx = hx, dy = y;
                switch (degrees) {
                    case 90: dx = crop.h - 1 - y; dy = hx; break;
                    case 180: dx = crop.w - 1 - hx; dy = crop.h - 1 - y; break;
                    case 270: dx = y; dy = crop.w - 1 - hx; break;
                }
                if (actual[dy * vw + dx] != source[(crop.y + y) * kPanelW + crop.x + x]) {
                    std::printf("     crop=%dx%d rotation=%d flip=%d mismatch=(%d,%d)\n",
                                crop.w, crop.h, degrees, flip, dx, dy);
                    equal = false;
                    break;
                }
            }
            check(equal, "奇偶尺寸裁剪的每个像素均先水平翻转再顺时针旋转，无偏移或黑边");
        }
    }
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
            const int cx = x - crop.x, cy = y - crop.y;
            if (cx >= 0 && cy >= 0 && cx < crop.w && cy < crop.h) {
                color = kBackground;
                const bool left = cx < crop.w / 4;
                const bool right = cx >= crop.w - crop.w / 4;
                const bool top = cy < crop.h / 4;
                const bool bottom = cy >= crop.h - crop.h / 4;
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
    spec.title = "Presenter geometry input regression";
    spec.want_w = crop.w;
    spec.want_h = crop.h;
    spec.want_readback = true;
    const bool opened = presenter.open(frame.width, frame.height, crop, 0, 1, false, spec);
    check(opened, "为 SDL 输入事件回归创建实际 Presenter");
    if (!opened) return;
    check(presenter.draw(frame, crop), "输入事件前绘制有效几何帧");
    const Uint32 window_id = window_id_from_events(spec.title.c_str());
    if (window_id == 0) return;

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
        // 窗口和视口都是 1:1，事件携带真实身份，坐标不需要额外缩放。
        if (type == SDL_MOUSEMOTION) {
            event.motion.windowID = window_id;
            event.motion.state = SDL_BUTTON_LMASK;
            event.motion.x = x;
            event.motion.y = y;
        } else {
            event.button.windowID = window_id;
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

void presenter_flip_mouse_events() {
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    for (const int pixel_degrees : {0, 90, 180, 270}) for (const int degrees : {0, 90, 180, 270}) {
        const bool swapped = pixel_degrees == 90 || pixel_degrees == 270;
        const int source_w = swapped ? 120 : 80, source_h = swapped ? 80 : 120;
        const auto crop = scrctl::app::make_frame_crop(true, 10, 12, 40, 60, source_w, source_h,
            scrctl::app::FrameGeometry{80, 120, pixel_degrees, true});
        const auto frame = colored_frame(source_w, source_h, crop, colors, 0);
        scrctl::app::WindowSpec spec;
        spec.title = "Presenter flip input regression";
        spec.horizontal_flip = true;
        spec.want_readback = true;
        scrctl::app::viewport_size(crop, degrees, spec.want_w, spec.want_h);
        scrctl::app::Presenter presenter;
        const bool opened = presenter.open(frame.width, frame.height, crop, degrees, 1, false, spec);
        check(opened, "为翻转鼠标回归创建实际 Presenter");
        if (!opened) continue;
        check(presenter.draw(frame, crop), "绘制带裁剪和截图像素方向的翻转帧");
        const Uint32 window_id = window_id_from_events(spec.title.c_str());
        if (window_id == 0) continue;
        struct Touch { double x, y; bool down; };
        std::vector<Touch> callbacks;
        const auto on_touch = [&](double x, double y, bool down) { callbacks.push_back({x, y, down}); };
        const auto push = [&](Uint32 type, Point source_point) {
            // 独立前向构造事件位置：裁剪内先水平翻转，再顺时针旋转。
            const int hx = crop.w - source_point.x;
            Point viewport{hx, source_point.y};
            switch (degrees) {
                case 90: viewport = {crop.h - source_point.y, hx}; break;
                case 180: viewport = {crop.w - hx, crop.h - source_point.y}; break;
                case 270: viewport = {source_point.y, crop.w - hx}; break;
            }
            SDL_Event event{};
            event.type = type;
            // 事件携带真实窗口身份；测试窗口与当前视口是 1:1。
            if (type == SDL_MOUSEMOTION) {
                event.motion.windowID = window_id;
                event.motion.state = SDL_BUTTON_LMASK;
                event.motion.x = viewport.x; event.motion.y = viewport.y;
            } else {
                event.button.windowID = window_id;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                event.button.x = viewport.x; event.button.y = viewport.y;
            }
            check(SDL_PushEvent(&event) == 1, "向实际 SDL 队列发送翻转后的指针位置");
        };
        const auto expected = [&](Point source_point) {
            const double x = double(crop.x + source_point.x) / source_w;
            const double y = double(crop.y + source_point.y) / source_h;
            switch (pixel_degrees) {
                case 90: return Touch{y, 1 - x, true};
                case 180: return Touch{1 - x, 1 - y, true};
                case 270: return Touch{1 - y, x, true};
                default: return Touch{x, y, true};
            }
        };
        const Point first{8, 18}, last{28, 42};
        push(SDL_MOUSEBUTTONDOWN, first);
        push(SDL_MOUSEMOTION, last);
        push(SDL_MOUSEBUTTONUP, last);
        check(!presenter.pump(on_touch), "实际 Presenter 处理翻转指针事件");
        check(callbacks.size() == 3, "翻转拖拽依次交付按下、合并移动和抬起");
        for (std::size_t i = 0; i < callbacks.size() && i < 3; ++i) {
            const auto want = expected(i == 0 ? first : last);
            check(callbacks[i].down == (i < 2) && std::fabs(callbacks[i].x - want.x) < 1e-9 &&
                  std::fabs(callbacks[i].y - want.y) < 1e-9,
                  "翻转窗口的设备回调还原裁剪偏移和截图自身方向");
        }
        if (pixel_degrees == 90 && degrees == 270) {
            callbacks.clear();
            push(SDL_MOUSEBUTTONDOWN, first); presenter.pump(on_touch);
            auto unknown = crop; unknown.input_valid = false;
            check(presenter.draw(frame, unknown), "未知截图几何仍显示翻转画面");
            push(SDL_MOUSEBUTTONDOWN, last); presenter.pump(on_touch);
            const auto want = expected(first);
            check(callbacks.size() == 2 && !callbacks.back().down &&
                  std::fabs(callbacks.back().x - want.x) < 1e-9 &&
                  std::fabs(callbacks.back().y - want.y) < 1e-9,
                  "翻转截图失去几何依据时按旧有效点释放，拒绝新按下");
        }
    }
}

void presenter_releases_touch_when_window_deactivates() {
    const scrctl::app::Crop crop{10, 20, 40, 60, 80, 120};
    scrctl::app::WindowSpec spec;
    spec.title = "Presenter focus lifecycle regression";
    spec.want_w = crop.w;
    spec.want_h = crop.h;
    spec.want_readback = true;
    scrctl::app::Presenter presenter;
    const bool opened = presenter.open(80, 120, crop, 0, 1, false, spec);
    check(opened, "创建窗口失活触摸回归的实际 Presenter");
    if (!opened) return;

    // 从 SDL 实际生成的窗口事件取得身份，不为测试开放 Presenter 私有窗口。
    const Uint32 own_id = window_id_from_events(spec.title.c_str());
    if (own_id == 0) return;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> foreign(
        SDL_CreateWindow("Unrelated focus lifecycle window", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, 40, 60, SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    check(foreign != nullptr, "创建另一窗口以验证失活事件的身份过滤");
    if (!foreign) return;
    const Uint32 foreign_id = SDL_GetWindowID(foreign.get());
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    struct Touch { double x, y; bool down; };
    std::vector<Touch> callbacks;
    const auto on_touch = [&](double x, double y, bool down) {
        callbacks.push_back({x, y, down});
    };
    const auto push_mouse_for = [&](Uint32 type, int x, int y, Uint32 window_id) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_MOUSEMOTION) {
            e.motion.windowID = window_id;
            e.motion.state = SDL_BUTTON_LMASK;
            e.motion.x = x; e.motion.y = y;
        } else {
            e.button.windowID = window_id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            e.button.x = x; e.button.y = y;
        }
        check(SDL_PushEvent(&e) == 1, "将带窗口身份的鼠标事件送入实际 SDL 队列");
    };
    const auto push_mouse = [&](Uint32 type, int x, int y) {
        push_mouse_for(type, x, y, own_id);
    };
    const auto push_window = [&](Uint8 state, Uint32 window_id) {
        SDL_Event e{};
        e.type = SDL_WINDOWEVENT;
        e.window.windowID = window_id;
        e.window.event = state;
        check(SDL_PushEvent(&e) == 1, "将带窗口身份的状态事件送入实际 SDL 队列");
    };
    const auto push_key = [&](SDL_Keycode symbol, Uint16 mods, Uint32 window_id) {
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.windowID = window_id;
        e.key.keysym.sym = symbol;
        e.key.keysym.mod = mods;
        check(SDL_PushEvent(&e) == 1, "将带窗口身份的快捷键事件送入实际 SDL 队列");
    };
    const auto matches = [&](std::size_t index, bool down, int x, int y) {
        return index < callbacks.size() && callbacks[index].down == down &&
               std::fabs(callbacks[index].x - double(crop.x + x) / crop.display_w) < 1e-9 &&
               std::fabs(callbacks[index].y - double(crop.y + y) / crop.display_h) < 1e-9;
    };
    for (const Uint8 state : {SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_HIDDEN,
                             SDL_WINDOWEVENT_MINIMIZED}) {
        callbacks.clear();
        push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
        push_mouse(SDL_MOUSEMOTION, 20, 30);
        push_window(state, own_id);
        // 鼠标系统可能仍有已经排队的尾部事件；它们不能在释放后恢复触摸。
        push_mouse(SDL_MOUSEBUTTONDOWN, 28, 42);
        push_mouse(SDL_MOUSEMOTION, 28, 42);
        push_mouse(SDL_MOUSEBUTTONUP, 28, 42);
        check(!presenter.pump(on_touch), "窗口失活释放触点而不退出程序");
        check(callbacks.size() == 2 && matches(0, true, 8, 12) &&
                  matches(1, false, 8, 12),
              "失活仅在最后已交付的点抬起一次，不跳到未发送的移动位置");
        push_window(SDL_WINDOWEVENT_FOCUS_GAINED, foreign_id);
        push_window(SDL_WINDOWEVENT_FOCUS_GAINED, 0);
        push_mouse(SDL_MOUSEBUTTONDOWN, 28, 42);
        push_mouse(SDL_MOUSEMOTION, 28, 42);
        push_mouse(SDL_MOUSEBUTTONUP, 28, 42);
        push_key(SDLK_F11, KMOD_NONE, own_id);
        push_key(SDLK_q, KMOD_LALT, own_id);
        check(!presenter.pump(on_touch) && !presenter.is_fullscreen() && callbacks.size() == 2,
              "失活持续到后续 pump，外部焦点事件不能恢复本窗口鼠标或快捷键");
        push_window(state, own_id);
        presenter.pump(on_touch);
        presenter.release_touch(on_touch);
        check(callbacks.size() == 2, "重复失活、pump 或显式释放不重复抬起");

        push_window(SDL_WINDOWEVENT_FOCUS_GAINED, own_id);
        push_mouse(SDL_MOUSEBUTTONDOWN, 4, 48);
        presenter.pump(on_touch);
        check(callbacks.size() == 3 && matches(2, true, 4, 48),
              "窗口重新获得焦点后新的按下仍可使用");
        presenter.release_touch(on_touch);
        check(callbacks.size() == 4 && matches(3, false, 4, 48),
              "新拖动仍以自身的最后有效点释放");

        callbacks.clear();
        push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
        push_mouse(SDL_MOUSEMOTION, 20, 30);
        push_window(state, foreign_id);
        push_window(state, 0);
        presenter.pump(on_touch);
        check(callbacks.size() == 2 && matches(0, true, 8, 12) &&
                  matches(1, true, 20, 30),
              "其他窗口和缺失 windowID 的事件不释放本窗口或丢弃其移动");
        push_mouse(SDL_MOUSEBUTTONUP, 20, 30);
        presenter.pump(on_touch);
        check(callbacks.size() == 3 && matches(2, false, 20, 30),
              "过滤外部失活事件后原拖动仍能正常抬起");
    }

    callbacks.clear();
    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    push_mouse(SDL_MOUSEMOTION, 20, 30);
    presenter.pump(on_touch);
    push_mouse(SDL_MOUSEMOTION, 28, 42);
    push_window(SDL_WINDOWEVENT_FOCUS_LOST, own_id);
    presenter.pump(on_touch);
    check(callbacks.size() == 3 && matches(0, true, 8, 12) &&
              matches(1, true, 20, 30) && matches(2, false, 20, 30),
          "前一轮已交付移动后失焦，沿用已交付点而非下一轮待发点抬起");

    push_window(SDL_WINDOWEVENT_FOCUS_GAINED, own_id);
    presenter.pump(on_touch);
    callbacks.clear();
    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    for (const Uint32 window_id : {foreign_id, Uint32{0}}) {
        push_mouse_for(SDL_MOUSEBUTTONDOWN, 28, 42, window_id);
        push_mouse_for(SDL_MOUSEMOTION, 28, 42, window_id);
        push_mouse_for(SDL_MOUSEBUTTONUP, 28, 42, window_id);
        push_key(SDLK_F11, KMOD_NONE, window_id);
        push_key(SDLK_q, KMOD_LALT, window_id);
    }
    check(!presenter.pump(on_touch) && !presenter.is_fullscreen() &&
              callbacks.size() == 1 && matches(0, true, 8, 12),
          "其他窗口和缺失身份的 mouse/key 不改变本窗口触点、全屏或退出状态");
    push_mouse(SDL_MOUSEBUTTONUP, 8, 12);
    presenter.pump(on_touch);
    check(callbacks.size() == 2 && matches(1, false, 8, 12),
          "外部鼠标抬起不能释放本窗口，原窗口仍可正常释放");

    callbacks.clear();
    push_mouse(SDL_MOUSEBUTTONDOWN, 8, 12);
    push_mouse(SDL_MOUSEMOTION, 20, 30);
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    check(SDL_PushEvent(&quit) == 1, "在未发送移动之后向 SDL 队列送入退出请求");
    push_window(SDL_WINDOWEVENT_FOCUS_GAINED, own_id);
    push_mouse(SDL_MOUSEBUTTONDOWN, 28, 42);
    push_mouse(SDL_MOUSEMOTION, 28, 42);
    push_mouse(SDL_MOUSEBUTTONUP, 28, 42);
    push_key(SDLK_F11, KMOD_NONE, own_id);
    check(presenter.pump(on_touch) && !presenter.is_fullscreen() && callbacks.size() == 2 &&
              matches(0, true, 8, 12) && matches(1, false, 8, 12),
          "退出后不恢复焦点或派发队列尾部，只在已交付位置释放一次");
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
    const Uint32 window_id = window_id_from_events(spec.title.c_str());
    if (window_id == 0) return;
    int presses = 0, releases = 0;
    const auto on_touch = [&](double, double, bool down) { down ? ++presses : ++releases; };
    const auto key = [&](SDL_Keycode symbol, Uint16 mods = KMOD_NONE, Uint8 repeat = 0) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.windowID = window_id;
        event.key.keysym.sym = symbol;
        event.key.keysym.mod = mods;
        event.key.repeat = repeat;
        check(SDL_PushEvent(&event) == 1, "将键盘事件送入实际 SDL 队列");
        return presenter.pump(on_touch);
    };
    SDL_Event down{};
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.windowID = window_id;
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
    const auto restore_focus = [&] {
        SDL_Event event{};
        event.type = SDL_WINDOWEVENT;
        event.window.windowID = window_id;
        event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
        check(SDL_PushEvent(&event) == 1 && !presenter.pump(on_touch),
              "独立检查下一快捷键之前显式恢复本窗口焦点");
    };
    restore_focus();
    check(key(SDLK_q, KMOD_LGUI), "默认左 Super+Q 退出");
    restore_focus();
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
    spec.title = "Presenter configured shortcut regression";
    spec.fullscreen = true;
    scrctl::app::Presenter configured;
    const bool opened = configured.open(64, 96, crop, 0, 1, false, spec);
    check(opened, "使用自定义修饰键创建全屏窗口");
    if (!opened) return;
    const Uint32 configured_id = window_id_from_events(spec.title.c_str());
    if (configured_id == 0) return;
    const auto configured_key = [&](Uint16 mods, SDL_Keycode symbol) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.windowID = configured_id;
        event.key.keysym.sym = symbol;
        event.key.keysym.mod = mods;
        check(SDL_PushEvent(&event) == 1, "送入自定义快捷键事件");
        return configured.pump({});
    };
    check(!configured_key(KMOD_LALT, SDLK_q), "自定义修饰键不再接受默认退出组合");
    configured_key(KMOD_RCTRL, SDLK_f);
    check(!configured.is_fullscreen(), "从启动全屏模式退出后仍可切换窗口模式");
    check(configured_key(KMOD_RCTRL, SDLK_q), "自定义修饰键替换默认退出组合");
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

    for (const int degrees : {0, 90, 180, 270}) for (const bool flip : {false, true}) {
        std::printf("== 顺时针旋转 %d°，水平翻转 %s ==\n", degrees, flip ? "是" : "否");
        std::vector<Uint32> px;
        int ow = 0, oh = 0;
        const int before = failures;
        if (!render_once(degrees, px, ow, oh, flip)) {
            ++failures;
            continue;
        }
        // 视口尺寸本身是一条判据：转 90/270 必须宽高对调，否则窗口比例就是错的。
        const int want_w = (degrees == 90 || degrees == 270) ? kPanelH : kPanelW;
        const int want_h = (degrees == 90 || degrees == 270) ? kPanelW : kPanelH;
        check(ow == want_w && oh == want_h, "视口尺寸按旋转对调");
        for (int i = 0; i < 4; ++i) {
            const Point want = viewport_corner(i, degrees, flip);
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
    cropped_flip_pixels();
    presenter_source_size_changes();
    presenter_releases_touch_when_geometry_changes();
    presenter_flip_mouse_events();
    presenter_releases_touch_when_window_deactivates();
    presenter_shortcuts_preserve_normal_input();

    SDL_Quit();
    if (failures != 0) {
        std::printf("render_test: %d 项失败\n", failures);
        return 1;
    }
    std::printf("render_test: 全部通过\n");
    return 0;
}
