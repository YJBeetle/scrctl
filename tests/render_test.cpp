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
/// 每档创建独立绘制面，使旋转公式的测试不依赖其它档的 renderer 状态。
/// 窗口复用和尺寸变化由下面的 Presenter 测试另行覆盖。
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
/// SDL_RenderClear 会清除整个目标面，留边也应具有指定背景色。
/// 回读时则要使用完整输出面的坐标：logical size 对应的 viewport 会影响
/// SDL_RenderReadPixels 的矩形，不能把物理输出宽高当作该 viewport 内的矩形。
/// 窗口与画面比例不同时，这两项才能同时覆盖真实留边和完整输出回读。
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
    check(!presenter.pump(on_touch), "恢复几何先清除旧坐标的输入队列");
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
    check(!presenter.pump(on_touch), "恢复面板尺寸先完成旧输入清理");
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
        e.key.keysym.scancode = SDL_GetScancodeFromKey(symbol);
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
        event.key.keysym.scancode = SDL_GetScancodeFromKey(symbol);
        event.key.keysym.mod = mods;
        event.key.repeat = repeat;
        check(SDL_PushEvent(&event) == 1, "将键盘事件送入实际 SDL 队列");
        if (!repeat) {
            event.type = SDL_KEYUP;
            event.key.state = SDL_RELEASED;
            check(SDL_PushEvent(&event) == 1, "同一次按键检查包含实际抬起事件");
        }
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
        event.key.keysym.scancode = SDL_GetScancodeFromKey(symbol);
        event.key.keysym.mod = mods;
        check(SDL_PushEvent(&event) == 1, "送入自定义快捷键事件");
        event.type = SDL_KEYUP;
        event.key.state = SDL_RELEASED;
        check(SDL_PushEvent(&event) == 1, "自定义快捷键检查包含按键抬起");
        return configured.pump({});
    };
    check(!configured_key(KMOD_LALT, SDLK_q), "自定义修饰键不再接受默认退出组合");
    configured_key(KMOD_RCTRL, SDLK_f);
    check(!configured.is_fullscreen(), "从启动全屏模式退出后仍可切换窗口模式");
    check(configured_key(KMOD_RCTRL, SDLK_q), "自定义修饰键替换默认退出组合");
}

struct KeyboardFixture {
    using Report = scrctl::app::KeyboardState::Report;
    using Reports = scrctl::app::KeyboardState::Reports;
    struct Touch { double x, y; bool down; };

    scrctl::app::Presenter presenter;
    scrctl::app::Crop crop{0, 0, 64, 96, 64, 96};
    scrctl::Frame frame;
    Uint32 window_id = 0;
    Reports reports;
    std::vector<Touch> touches;

    explicit KeyboardFixture(const char *title, int width = 64, int height = 96,
                             int degrees = 0, Uint16 shortcut_mods = KMOD_LALT | KMOD_LGUI,
                             bool background = false, bool horizontal_flip = false) {
        const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
        if (!background) frame = colored_frame(64, 96, crop, colors, 0);
        scrctl::app::WindowSpec spec;
        spec.title = title;
        spec.want_w = width;
        spec.want_h = height;
        spec.want_readback = true;
        spec.shortcut_mods = shortcut_mods;
        spec.horizontal_flip = horizontal_flip;
        const bool opened = background ? presenter.open_background(spec)
                                      : presenter.open(64, 96, crop, degrees, 1, false, spec);
        check(opened, "为输入回归创建实际 Presenter");
        if (opened) window_id = window_id_from_events(title);
    }

    auto on_touch() {
        return [this](double x, double y, bool down) { touches.push_back({x, y, down}); };
    }
    auto on_keyboard() {
        return [this](const Report &report) { reports.push_back(report); };
    }
    bool pump() { return presenter.pump(on_touch(), on_keyboard()); }
    void release() { presenter.release_input(on_touch(), on_keyboard()); }
    void key_for(Uint32 type, SDL_Scancode scancode, Uint16 mods, Uint8 repeat, Uint32 id) {
        SDL_Event event{};
        event.type = type;
        event.key.windowID = id;
        event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.scancode = scancode;
        event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
        event.key.keysym.mod = mods;
        event.key.repeat = repeat;
        check(SDL_PushEvent(&event) == 1, "将物理键盘事件送入实际 SDL 队列");
    }
    void key(Uint32 type, SDL_Scancode scancode, Uint16 mods = KMOD_NONE, Uint8 repeat = 0) {
        key_for(type, scancode, mods, repeat, window_id);
    }
    void window(Uint8 state, Uint32 id) {
        SDL_Event event{};
        event.type = SDL_WINDOWEVENT;
        event.window.windowID = id;
        event.window.event = state;
        check(SDL_PushEvent(&event) == 1, "将键盘生命周期窗口事件送入实际 SDL 队列");
    }
    void window(Uint8 state) { window(state, window_id); }
    void mouse(Uint32 type, int x, int y, Uint8 clicks = 1, Uint32 which = 0, Uint32 id = 0) {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_MOUSEMOTION) {
            event.motion.windowID = window_id;
            event.motion.state = SDL_BUTTON_LMASK;
            event.motion.x = x; event.motion.y = y;
        } else {
            event.button.windowID = id ? id : window_id;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.button.x = x; event.button.y = y;
            event.button.clicks = clicks;
            event.button.which = which;
        }
        check(SDL_PushEvent(&event) == 1, "向键盘生命周期窗口送入触摸事件");
    }
    void expect(const Reports &expected, const char *message) {
        check(reports == expected, message);
        if (reports != expected) {
            for (const auto &report : reports) {
                std::printf("     keyboard report:");
                for (const auto usage : report) std::printf(" %u", usage);
                std::printf("\n");
            }
        }
        reports.clear();
    }
};


void presenter_display_rotation() {
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    // 每一行是旋转后的屏幕 TL/TR/BR/BL 对应原角块；判据不用生产坐标变换。
    constexpr int corner_order[2][4][4] = {
        {{0,1,2,3}, {3,0,1,2}, {2,3,0,1}, {1,2,3,0}},
        {{1,0,3,2}, {2,1,0,3}, {3,2,1,0}, {0,3,2,1}}
    };
    for (const bool flip : {false, true}) {
        KeyboardFixture f("Presenter display rotation pixels", 82, 126, 0,
                          KMOD_LALT | KMOD_LGUI, false, flip);
        if (!f.window_id) continue;
        auto *window = SDL_GetWindowFromID(f.window_id);
        auto *renderer = window ? SDL_GetRenderer(window) : nullptr;
        if (!renderer) continue;
        f.crop = {7, 9, 41, 63, 96, 128};
        f.frame = colored_frame(96, 128, f.crop, colors, 12);
        check(f.presenter.update_content(f.crop, 0, f.on_touch(), f.on_keyboard()) &&
                  f.presenter.draw(f.frame, f.crop) && !f.pump(),
              "建立不居中奇数裁剪的真实纹理及已知输入依据");
        SDL_SetWindowSize(window, 82, 126);
        SDL_SetWindowPosition(window, 70, 90);
        check(!f.pump() && f.presenter.draw(f.frame), "建立用户选择的二倍显示尺度和位置");
        f.reports.clear(); f.touches.clear();
        for (unsigned quarter = 1; quarter <= 4; ++quarter) {
            const auto generation = f.presenter.input_generation();
            f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
            f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
            check(!f.pump() && !f.presenter.render_failed() &&
                      f.presenter.display_degrees(0) == int(quarter % 4) * 90 &&
                      f.presenter.input_generation() == generation + 1,
                  "实际 MOD+Right 每次追加顺时针90度并仅失效一次旧粘贴代次");
            int w = 0, h = 0, x = 0, y = 0;
            SDL_GetWindowSize(window, &w, &h); SDL_GetWindowPosition(window, &x, &y);
            check(w == (quarter % 2 ? 126 : 82) && h == (quarter % 2 ? 82 : 126) &&
                      x == 70 && y == 90 && SDL_GetRenderer(window) == renderer,
                  "快捷键原地交换可见宽高，保留用户尺度、位置和renderer");
            std::vector<Uint32> pixels(static_cast<std::size_t>(w) * h);
            const SDL_Rect full{0,0,w,h};
            check(SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888,
                                      pixels.data(), w * int(sizeof(Uint32))) == 0,
                  "旋转之后没有新draw或Frame，立即回读真实已上传纹理");
            const Point points[] = {{w/8,h/8}, {w-w/8,h/8}, {w-w/8,h-h/8}, {w/8,h-h/8}};
            for (unsigned screen_corner = 0; screen_corner < 4; ++screen_corner) {
                const auto source_corner = corner_order[flip ? 1 : 0][quarter % 4][screen_corner];
                const auto point = points[screen_corner];
                check(close(at(pixels.data(), w, point.x, point.y), colors[source_corner]),
                      "静止画面旋转四角和原CLI水平翻转同时正确，无编码填充泄漏");
                f.mouse(SDL_MOUSEBUTTONDOWN, point.x, point.y);
                f.mouse(SDL_MOUSEBUTTONUP, point.x, point.y);
                check(!f.pump() && f.touches.size() == 2 && !f.touches.back().down,
                      "旋转之后的新点击经过同一实际SDL事件路径");
                const bool left = source_corner == 0 || source_corner == 3;
                const bool top = source_corner < 2;
                const double expected_x = (f.crop.x + (left ? f.crop.w/8 : f.crop.w-f.crop.w/8)) / 96.0;
                const double expected_y = (f.crop.y + (top ? f.crop.h/8 : f.crop.h-f.crop.h/8)) / 128.0;
                check(f.touches.size() == 2 && std::abs(f.touches[0].x - expected_x) < 0.025 &&
                          std::abs(f.touches[0].y - expected_y) < 0.025,
                      "触点逆变换回到回读角块的原设备位置，含偏移裁剪及翻转");
                f.touches.clear();
            }
            f.expect({}, "四次本地旋转及其UP不进入设备键盘");
        }
        f.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_LEFT);
        check(!f.pump() && f.presenter.display_degrees(0) == 270,
              "MOD+Left 向左90度而不是再次顺时针");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!f.pump() && f.presenter.display_degrees(-360) == 0 &&
                  f.presenter.display_degrees(450) == 90,
              "左右互逆且公开方向查询规范化负值和超一圈基准");
        SDL_SetWindowSize(window, 180, 180); check(!f.pump(), "恢复有留边窗口检查旋转输入边界");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!f.pump(), "留边窗口也能立即本机旋转");
        // 内容适配本身会去掉原留边；用户再调为方形后，检查新方向下的留边。
        SDL_SetWindowSize(window, 180, 180); check(!f.pump(), "旋转后用户再次调整为方形窗口");
        f.mouse(SDL_MOUSEBUTTONDOWN, 90, 0); f.mouse(SDL_MOUSEBUTTONUP, 90, 0);
        check(!f.pump() && f.touches.empty(), "旋转后的真实内容留边拒绝触摸，不夹到设备边缘");
    }

    KeyboardFixture f("Presenter display rotation key ownership");
    if (!f.window_id) return;
    check(f.presenter.draw(f.frame) && !f.pump(), "建立旋转键盘和触点生命周期的实际纹理");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A); f.mouse(SDL_MOUSEBUTTONDOWN, 16, 24);
    check(!f.pump(), "旋转前已有实际设备键和已交付触点");
    f.mouse(SDL_MOUSEMOTION, 48, 72);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
    f.mouse(SDL_MOUSEBUTTONDOWN, 20, 30); f.mouse(SDL_MOUSEMOTION, 30, 40);
    f.mouse(SDL_MOUSEBUTTONUP, 30, 40);
    f.key(SDL_KEYUP, SDL_SCANCODE_LALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_NONE, 1);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT);
    f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_B); f.key(SDL_KEYUP, SDL_SCANCODE_B);
    check(!f.pump() && f.presenter.display_degrees(0) == 90 && f.touches.size() == 2 &&
              f.touches[0].down && !f.touches[1].down &&
              f.touches[0].x == f.touches[1].x && f.touches[0].y == f.touches[1].y,
          "旋转只抬起最后实际交付旧触点，丢同轮pending_move和后续旧鼠标，不补DOWN");
    f.expect({{4}, {}, {5}, {}}, "本地箭头重复DOWN和先松MOD不泄漏，真实UP与后续普通B仍处理");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
    check(!f.pump() && f.presenter.display_degrees(0) == 180,
          "旋转保留Local到真实UP，新一趟同一箭头DOWN可以再次旋转");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT); check(!f.pump(), "普通Left交给设备");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, KMOD_LALT, 1);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, KMOD_LALT);
    check(!f.pump() && f.presenter.display_degrees(0) == 180,
          "已归设备的箭头后来按MOD或重复DOWN不能变成本机动作");
    f.key(SDL_KEYUP, SDL_SCANCODE_LEFT); check(!f.pump(), "设备箭头完整松开");
    f.expect({{80}, {}}, "设备所有权保持到UP且只产生一次报告");
    const auto unchanged = f.presenter.input_generation();
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT | KMOD_LSHIFT);
    f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
    f.key_for(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT, 0, f.window_id + 1000);
    f.key_for(SDL_KEYUP, SDL_SCANCODE_RIGHT, KMOD_NONE, 0, f.window_id + 1000);
    check(!f.pump() && f.presenter.display_degrees(0) == 180 &&
              f.presenter.input_generation() == unchanged + 1,
          "Shift水平翻转释放旧布局而不追加旋转，其他窗口箭头不能误触发");
    f.expect({}, "本地Shift翻转组合也不泄漏设备键");
    f.window(SDL_WINDOWEVENT_FOCUS_LOST); f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
    check(!f.pump() && f.presenter.display_degrees(0) == 180, "失焦后不执行旋转");
    f.window(SDL_WINDOWEVENT_FOCUS_GAINED); check(!f.pump(), "恢复旋转窗口焦点");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_RALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
    check(!f.pump() && f.presenter.display_degrees(0) == 180, "默认AltGr组合保持设备输入，不旋转");
    f.expect({{230}, {79,230}, {}}, "右Alt与普通箭头按设备HID状态发送");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, KMOD_LALT | KMOD_RCTRL);
    f.key(SDL_KEYUP, SDL_SCANCODE_LEFT);
    check(!f.pump() && f.presenter.display_degrees(0) == 90,
          "选定MOD加额外Ctrl仍按scrcpy规则旋转，无Shift时不执行镜像");
    f.expect({}, "本地旋转清理伴随的设备修饰键");

    auto *window = SDL_GetWindowFromID(f.window_id);
    SDL_SetWindowSize(window, 144, 96); check(!f.pump(), "设置特殊模式前的用户尺度");
    check(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 &&
              f.presenter.is_fullscreen(), "实际SDL后端进入全屏模式而非仅合成事件");
    if (f.presenter.is_fullscreen()) {
        const auto generation = f.presenter.input_generation();
        for (unsigned i = 0; i < 3; ++i) {
            f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
            if (i < 2) f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
            check(!f.pump() && f.presenter.is_fullscreen(), "全屏多次旋转保留窗口模式");
        }
        check(f.presenter.display_degrees(0) == 0, "全屏方向只保留最终偏移");
        check(SDL_SetWindowFullscreen(window, 0) == 0, "真实SDL窗口退出全屏");
        f.window(SDL_WINDOWEVENT_RESTORED);
        f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!f.pump() && f.presenter.input_generation() == generation + 4,
              "恢复时仅最终一次尺寸适配且保留Local箭头UP到状态机");
        int w = 0, h = 0; SDL_GetWindowSize(window, &w, &h);
        check(w == 96 && h == 144, "恢复普通窗口按最初基准和最后方向适配用户尺度");
        const auto restored = f.presenter.input_generation();
        check(!f.pump() && f.presenter.input_generation() == restored,
              "已完成延后适配不反复清理输入或缩放窗口");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!f.pump() && f.presenter.display_degrees(0) == 90,
              "延后适配没有丢Local UP，恢复后同键新DOWN继续旋转");
    }

    {
        KeyboardFixture mixed("Presenter deferred rotation source precedence");
        if (mixed.window_id) {
            auto *window = SDL_GetWindowFromID(mixed.window_id);
            check(mixed.presenter.draw(mixed.frame) && !mixed.pump(), "建立混合延后布局的已上传纹理");
            check(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 &&
                      mixed.presenter.is_fullscreen(), "混合清理判据使用实际全屏flag");
            if (mixed.presenter.is_fullscreen()) {
                mixed.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
                check(!mixed.pump(), "本地旋转保存Local归属并建立延后尺寸");
                mixed.key(SDL_KEYDOWN, SDL_SCANCODE_UP, KMOD_LALT | KMOD_LSHIFT);
                mixed.key(SDL_KEYUP, SDL_SCANCODE_UP);
                check(!mixed.pump(), "延后尺寸中的V镜像复用Local清理且不替换来源优先级");
                mixed.key(SDL_KEYDOWN, SDL_SCANCODE_A); check(!mixed.pump(), "来源变化之前已有设备A");
                auto changed = mixed.crop; changed.pixel_degrees = 90;
                check(mixed.presenter.draw(mixed.frame, changed), "全屏内合法新Frame改变源坐标依据");
                check(SDL_SetWindowFullscreen(window, 0) == 0, "混合来源清理前恢复普通窗口");
                mixed.window(SDL_WINDOWEVENT_RESTORED);
                mixed.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
                mixed.key(SDL_KEYDOWN, SDL_SCANCODE_B); mixed.key(SDL_KEYUP, SDL_SCANCODE_B);
                check(!mixed.pump(), "来源失效优先于本地延后保留，不处理旧队列输入");
                mixed.expect({{4}, {}}, "新源几何执行原完整释放和KEY队列过滤，不伪装成纯本地动作");
                mixed.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); mixed.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
                check(!mixed.pump() && mixed.presenter.display_degrees(0) == 180,
                      "完整来源清理后新箭头仍可重新取得本地归属");
            }
        }
    }
    KeyboardFixture configured("Presenter configured rotation", 64, 96, 0, KMOD_RCTRL);
    if (configured.window_id) {
        check(configured.presenter.draw(configured.frame) && !configured.pump(), "建立自定义修饰键的已上传纹理");
        configured.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
        configured.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!configured.pump() && configured.presenter.display_degrees(0) == 0,
              "自定义MOD替换默认旋转修饰键");
        configured.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_RCTRL);
        configured.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!configured.presenter.pump({}) && configured.presenter.display_degrees(0) == 90,
              "自定义右Ctrl生效，无设备回调或文件播放也能旋转");
    }
    KeyboardFixture blank("Presenter unuploaded rotation");
    if (blank.window_id) {
        auto *renderer = SDL_GetRenderer(SDL_GetWindowFromID(blank.window_id));
        SDL_SetRenderDrawColor(renderer, 233, 5, 9, 255); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
        blank.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); blank.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!blank.pump() && blank.presenter.display_degrees(0) == 90 && !blank.presenter.render_failed(),
              "未上传纹理只保存旋转状态，不读取未初始化像素");
        check(blank.presenter.draw(blank.frame) && !blank.pump(), "首个有效Frame按之前的用户方向绘制");
    }
    KeyboardFixture stale("Presenter rotated upload validity", 64, 64);
    if (stale.window_id) {
        stale.crop = {0,0,64,64,80,80};
        stale.frame = colored_frame(80, 80, stale.crop, colors, 0);
        check(stale.presenter.update_content(stale.crop, 0, stale.on_touch(), stale.on_keyboard()) &&
                  stale.presenter.draw(stale.frame, stale.crop) && !stale.pump(),
              "建立同尺寸来源变化前的真实合法已上传纹理");
        stale.window(SDL_WINDOWEVENT_MINIMIZED); check(!stale.pump(), "真实最小化事件暂停显示上传");
        const Palette changed_colors{kBottomRight,kTopLeft,kBottomLeft,kTopRight};
        stale.crop = {8,8,64,64,80,80}; stale.crop.pixel_degrees = 90;
        stale.frame = colored_frame(80, 80, stale.crop, changed_colors, 0);
        check(stale.presenter.draw(stale.frame, stale.crop),
              "最小化期间同纹理尺寸的新crop和pixel依据跳过上传，不把旧像素标为新图像");
        stale.window(SDL_WINDOWEVENT_RESTORED); stale.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        check(!stale.pump(), "恢复时释放源几何旧输入，但未提供新Frame");
        auto *window = SDL_GetWindowFromID(stale.window_id);
        SDL_SetWindowSize(window, 64, 64); check(!stale.pump(), "保持方形绘制面使有效性判据不受resize影响");
        auto *renderer = SDL_GetRenderer(window);
        check(SDL_SetRenderDrawColor(renderer, 233,5,9,255) == 0 && SDL_RenderClear(renderer) == 0,
              "向真实绘制面写入独立marker以检测不该发生的旧纹理重绘");
        SDL_RenderPresent(renderer);
        stale.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); stale.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!stale.pump() && stale.presenter.display_degrees(0) == 90,
              "恢复后无新Frame仍可更新旋转状态");
        std::vector<Uint32> pixels(64*64); const SDL_Rect full{0,0,64,64};
        check(SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888, pixels.data(), 64*4) == 0 &&
                  std::all_of(pixels.begin(), pixels.end(), [](Uint32 pixel) { return pixel == 0xffe90509; }),
              "旧纹理在同size源几何变化后已失效，旋转不覆盖完整marker或读取未上传像素");
        check(stale.presenter.draw(stale.frame, stale.crop) && !stale.pump(),
              "来源恢复合法上传后再次允许即时纹理重绘");
        stale.key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, KMOD_LALT); stale.key(SDL_KEYUP, SDL_SCANCODE_LEFT);
        check(!stale.pump() && SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888,
                   pixels.data(), 64*4) == 0 && close(at(pixels.data(),64,8,8), changed_colors[0]),
              "恢复上传后的静止画面左转呈现新crop角块，而非旧Frame");
        auto next = stale.crop; next.pixel_degrees = 180;
        SDL_SetRenderDrawColor(renderer, 233,5,9,255); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
        const auto generation = stale.presenter.input_generation();
        check(stale.presenter.update_content(next, 0, stale.on_touch(), stale.on_keyboard()) &&
                  stale.presenter.input_generation() == generation,
              "同视口来源变更保留旧布局无操作语义，同时撤销旧上传有效性");
        stale.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); stale.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!stale.pump() && SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888,
                   pixels.data(),64*4) == 0 && std::all_of(pixels.begin(),pixels.end(),
                   [](Uint32 pixel) { return pixel == 0xffe90509; }),
              "相同degree及viewport早返回也不会让旧纹理跨来源依据继续重绘");
    }
    KeyboardFixture background("Presenter background rotation disabled", 100, 80, 0, KMOD_LALT, true);
    if (background.window_id) {
        const auto generation = background.presenter.input_generation();
        background.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
        background.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
        check(!background.pump() && background.presenter.display_degrees(0) == 0 &&
                  background.presenter.input_generation() == generation,
              "无视频背景窗口不执行旋转或制造几何状态");
        background.expect({}, "背景MOD箭头仍归本地，不泄漏普通箭头");
    }
    {
        KeyboardFixture retained("Presenter rotation queue retention");
        if (retained.window_id) {
            std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> foreign(
                SDL_CreateWindow("Rotation foreign queue", 0,0,32,32,SDL_WINDOW_HIDDEN), SDL_DestroyWindow);
            check(foreign != nullptr, "建立实际其他窗口检查pointer过滤身份");
            if (foreign) {
                check(retained.presenter.draw(retained.frame) && !retained.pump(), "建立队列保留的真实纹理");
                retained.key(SDL_KEYDOWN, SDL_SCANCODE_A); check(!retained.pump(), "队列过滤前已有设备键");
                retained.reports.clear();
                SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
                const auto other_id = SDL_GetWindowID(foreign.get());
                retained.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT);
                retained.mouse(SDL_MOUSEBUTTONDOWN, 20,30);
                retained.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
                retained.key(SDL_KEYDOWN, SDL_SCANCODE_B);
                SDL_version linked{}; SDL_GetVersion(&linked);
                // 已复现的 sdl2-compat 2.32.72/74 合成 TEXTINPUT 转换返回 NULL，
                // PushEvent 随后在 SDL3 内崩溃；不能把危险注入用作本进程能力探测。
                const bool text_push_supported = !(linked.major == 2 && linked.minor == 32 &&
                                                    (linked.patch == 72 || linked.patch == 74));
                if (text_push_supported) {
                    SDL_Event text{}; text.type = SDL_TEXTINPUT; text.text.windowID = retained.window_id;
                    std::strcpy(text.text.text, "kept"); check(SDL_PushEvent(&text) == 1, "旋转后排入本窗口TEXT");
                }
                retained.key(SDL_KEYDOWN, SDL_SCANCODE_C); retained.key(SDL_KEYUP, SDL_SCANCODE_C);
                retained.key_for(SDL_KEYDOWN, SDL_SCANCODE_X, KMOD_NONE, 0, other_id);
                retained.mouse(SDL_MOUSEBUTTONDOWN, 4,5,1,0,other_id);
                retained.window(SDL_WINDOWEVENT_EXPOSED);
                retained.window(SDL_WINDOWEVENT_FOCUS_LOST, other_id);
                SDL_Event quit{}; quit.type = SDL_QUIT; check(SDL_PushEvent(&quit) == 1, "过滤判据包含全局QUIT");
                bool kept_text = false, kept_key = false, kept_window = false;
                bool kept_other_pointer = false, kept_other_key = false, kept_quit = false;
                bool kept_own_pointer = false;
                const auto keyboard = [&](const KeyboardFixture::Report &report) {
                    retained.reports.push_back(report);
                    if (report != KeyboardFixture::Report{5}) return;
                    std::array<SDL_Event,32> queue{};
                    const int count = SDL_PeepEvents(queue.data(), int(queue.size()), SDL_PEEKEVENT,
                                                    SDL_FIRSTEVENT, SDL_LASTEVENT);
                    for (int i=0; i<count; ++i) {
                        const auto &event = queue[static_cast<std::size_t>(i)];
                        kept_text |= event.type == SDL_TEXTINPUT && event.text.windowID == retained.window_id;
                        kept_key |= event.type == SDL_KEYDOWN && event.key.windowID == retained.window_id;
                        kept_window |= event.type == SDL_WINDOWEVENT && event.window.windowID == retained.window_id;
                        kept_other_key |= event.type == SDL_KEYDOWN && event.key.windowID == other_id;
                        kept_other_pointer |= event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == other_id;
                        kept_own_pointer |= event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == retained.window_id;
                        kept_quit |= event.type == SDL_QUIT;
                    }
                };
                check(retained.presenter.pump(retained.on_touch(), keyboard) && kept_key &&
                          kept_window && kept_other_key && kept_other_pointer && kept_quit && !kept_own_pointer,
                      "旋转仅删除本窗口旧pointer，真实queue保留KEY/WINDOW/QUIT及其他窗口输入");
                if (text_push_supported) check(kept_text, "实际TEXT队列也保留，不作为旧坐标输入删除");
                else std::printf("  SKIP SDL %u.%u.%u (%s) 不支持合成TEXTINPUT的PushEvent；KEY/WINDOW/QUIT/pointer断言仍执行\n",
                                 linked.major, linked.minor, linked.patch, SDL_GetRevision());
                check(retained.touches.empty(), "本窗口旧坐标及其他窗口pointer均不制造本窗口触摸");
                retained.expect({{}, {5}, {5,6}, {5}, {}},
                                "释放设备旧键后继续处理队列键盘，QUIT再释放实际新键一次");
            }
        }
    }
    f.key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT, KMOD_LALT); f.key(SDL_KEYUP, SDL_SCANCODE_RIGHT);
    SDL_Event quit{}; quit.type = SDL_QUIT; check(SDL_PushEvent(&quit) == 1, "旋转后排入真实QUIT");
    check(f.pump() && !f.presenter.render_failed(), "仅过滤旧pointer，保留QUIT以正常退出");
}

void presenter_display_flips() {
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    // 固定源角块表；H/V 的判据只交换当前显示左右/上下，不抄生产degree组合。
    constexpr int initial_order[2][4][4] = {
        {{0,1,2,3}, {3,0,1,2}, {2,3,0,1}, {1,2,3,0}},
        {{1,0,3,2}, {2,1,0,3}, {3,2,1,0}, {0,3,2,1}}
    };
    constexpr int mirror_corner[2][4] = {{1,0,3,2}, {3,2,1,0}};
    const auto corners = [&](KeyboardFixture &f, const std::array<int,4> &order) {
        auto *renderer = SDL_GetRenderer(SDL_GetWindowFromID(f.window_id));
        int w = 0, h = 0;
        const bool output = renderer && SDL_GetRendererOutputSize(renderer, &w, &h) == 0 &&
                            w > 0 && h > 0 && w <= 1024 && h <= 1024;
        check(output, "本机翻转使用实际有界SDL绘制面，不构造替代Frame");
        if (!output) return;
        std::vector<Uint32> pixels(static_cast<std::size_t>(w) * h);
        const SDL_Rect full{0,0,w,h};
        const bool read = SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888,
                                               pixels.data(), w * 4) == 0;
        check(read, "翻转后直接回读已上传纹理，无新的draw或Frame");
        if (!read) return;
        const Point points[] = {{w/8,h/8}, {w-w/8,h/8}, {w-w/8,h-h/8}, {w/8,h-h/8}};
        for (unsigned i = 0; i < 4; ++i) {
            check(close(at(pixels.data(), w, points[i].x, points[i].y), colors[order[i]]),
                  "回读四角符合当前显示轴的独立水平/垂直镜像判据");
        }
    };
    for (const bool cli_flip : {false,true}) for (const int base : {0,90,180,270})
        for (const bool vertical : {false,true}) {
            const bool swapped = base == 90 || base == 270;
            KeyboardFixture f("Presenter horizontal and vertical flip pixels",
                              swapped ? 126 : 82, swapped ? 82 : 126, base,
                              KMOD_LALT | KMOD_LGUI, false, cli_flip);
            if (!f.window_id) continue;
            auto *window = SDL_GetWindowFromID(f.window_id);
            auto *renderer = SDL_GetRenderer(window);
            // 源像素是96x128，截图已转90度，因此真实设备面板是128x96。
            f.crop = {7,9,41,63,128,96}; f.crop.pixel_degrees = 90;
            f.frame = colored_frame(96,128,f.crop,colors,12);
            check(f.presenter.update_content(f.crop, base, f.on_touch(), f.on_keyboard()),
                  "建立奇数偏心裁剪、CLI方向和截图pixel方向的翻转依据");
            SDL_SetWindowSize(window, swapped ? 126 : 82, swapped ? 82 : 126);
            SDL_SetWindowPosition(window, 70,90);
            check(!f.pump() && f.presenter.draw(f.frame,f.crop) && !f.pump(),
                  "翻转回读前仅上传一次带行填充的合法源Frame");
            f.reports.clear(); f.touches.clear();
            const auto generation = f.presenter.input_generation();
            const auto first = vertical ? SDL_SCANCODE_UP : SDL_SCANCODE_LEFT;
            const auto second = vertical ? SDL_SCANCODE_DOWN : SDL_SCANCODE_RIGHT;
            f.key(SDL_KEYDOWN, first, KMOD_LALT | KMOD_LSHIFT); f.key(SDL_KEYUP, first);
            const int wanted_degrees = ((vertical ? 180 : 0) - base + 360) % 360;
            check(!f.pump() && f.presenter.display_degrees(base) == wanted_degrees &&
                      f.presenter.input_generation() == generation + 1 && !f.presenter.render_failed(),
                  "H/V在当前显示轴镜像，0/180同尺寸也真实清理并立即重绘一次");
            int w = 0, h = 0, x = 0, y = 0;
            SDL_GetWindowSize(window,&w,&h); SDL_GetWindowPosition(window,&x,&y);
            check(w == (swapped ? 126 : 82) && h == (swapped ? 82 : 126) &&
                      x == 70 && y == 90 && SDL_GetRenderer(window) == renderer,
                  "水平/垂直镜像均保留窗口尺寸、用户尺度、位置和renderer身份");
            std::array<int,4> wanted{};
            for (unsigned i = 0; i < 4; ++i)
                wanted[i] = initial_order[cli_flip ? 1 : 0][base/90][mirror_corner[vertical ? 1 : 0][i]];
            corners(f,wanted);
            const Point points[] = {{w/8,h/8}, {w-w/8,h/8}, {w-w/8,h-h/8}, {w/8,h-h/8}};
            for (unsigned i = 0; i < 4; ++i) {
                f.mouse(SDL_MOUSEBUTTONDOWN,points[i].x,points[i].y);
                f.mouse(SDL_MOUSEBUTTONUP,points[i].x,points[i].y);
                check(!f.pump() && f.touches.size() == 2 && !f.touches.back().down,
                      "翻转四角点击经过实际SDL queue和Presenter设备回调");
                const bool left = wanted[i] == 0 || wanted[i] == 3, top = wanted[i] < 2;
                const double ix = (f.crop.x + (left ? f.crop.w/8 : f.crop.w-f.crop.w/8))/96.0;
                const double iy = (f.crop.y + (top ? f.crop.h/8 : f.crop.h-f.crop.h/8))/128.0;
                check(f.touches.size() == 2 && std::abs(f.touches[0].x-iy) < 0.025 &&
                          std::abs(f.touches[0].y-(1-ix)) < 0.025,
                      "触点还原回读原角块，保留偏心crop并逆截图已应用的90度方向");
                f.touches.clear();
            }
            f.key(SDL_KEYDOWN, second, KMOD_LGUI | KMOD_RSHIFT); f.key(SDL_KEYUP,second);
            check(!f.pump() && f.presenter.display_degrees(base) == base &&
                      f.presenter.input_generation() == generation + 2,
                  "左右H以及上下V分别等价，连续两次镜像回到原CLI/来源方向");
            std::array<int,4> original{};
            std::copy_n(initial_order[cli_flip ? 1 : 0][base/90],4,original.begin());
            corners(f,original);
            f.expect({}, "H/V及真实UP始终归本地，不向设备注入箭头或Shift");
        }
    {
        KeyboardFixture f("Presenter flip and source base composition",96,64,90,
                          KMOD_LALT | KMOD_LGUI,false,true);
        if (f.window_id) {
            check(f.presenter.draw(f.frame) && !f.pump(), "建立90度CLI镜像基准的合法纹理");
            f.key(SDL_KEYDOWN,SDL_SCANCODE_LEFT,KMOD_LALT | KMOD_LSHIFT);
            f.key(SDL_KEYUP,SDL_SCANCODE_LEFT);
            check(!f.pump() && f.presenter.display_degrees(90) == 270 &&
                      f.presenter.display_degrees(180) == 180 &&
                      f.presenter.display_degrees(-270) == 270,
                  "H撤销原源flip并反转后续基准，公开方向查询规范化负值");
            check(f.presenter.update_content(f.crop,f.presenter.display_degrees(180),f.on_touch(),f.on_keyboard()) &&
                      f.presenter.draw(f.frame) && !f.pump(),
                  "来源/显式方向改为180后，新Frame继续使用持久本机镜像偏移");
            corners(f,{2,3,0,1});
            f.key(SDL_KEYDOWN,SDL_SCANCODE_UP,KMOD_LALT | KMOD_LSHIFT); f.key(SDL_KEYUP,SDL_SCANCODE_UP);
            check(!f.pump() && f.presenter.display_degrees(180) == 0 &&
                      f.presenter.display_degrees(90) == 270,
                  "先H后V组合成180度本机旋转，后续基准重新同向相加");
            corners(f,{1,0,3,2});
        }
        KeyboardFixture order("Presenter rotate mirror noncommuting");
        if (order.window_id) {
            check(order.presenter.draw(order.frame) && !order.pump(), "建立旋转镜像不交换的真实纹理");
            for (const Uint16 mods : {Uint16(KMOD_LALT),Uint16(KMOD_LALT | KMOD_LSHIFT),Uint16(KMOD_LALT)}) {
                order.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,mods); order.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
                check(!order.pump(), "实际Right→H→Right组合经过同一事件循环");
            }
            check(order.presenter.display_degrees(0) == 0 && order.presenter.display_degrees(90) == 270,
                  "Right→H→Right等价当前水平镜像，不错误累加180度");
            corners(order,{1,0,3,2});
        }
    }
    for (const bool vertical : {false,true}) {
        KeyboardFixture f("Presenter flip keyboard and pointer ownership",64,96,vertical ? 180 : 0);
        if (!f.window_id) continue;
        auto *window = SDL_GetWindowFromID(f.window_id);
        check(f.presenter.draw(f.frame) && !f.pump(), "建立同尺寸H/V触点与键盘清理的真实纹理");
        const auto arrow = vertical ? SDL_SCANCODE_UP : SDL_SCANCODE_RIGHT;
        f.key(SDL_KEYDOWN,SDL_SCANCODE_A);
        f.key(SDL_KEYDOWN,SDL_SCANCODE_LSHIFT,KMOD_LSHIFT);
        f.mouse(SDL_MOUSEBUTTONDOWN,16,24); check(!f.pump(), "翻转前已交付设备A、Shift和旧触点");
        const auto generation = f.presenter.input_generation();
        f.mouse(SDL_MOUSEMOTION,48,72); // 尚未交付的合并移动不能影响旧UP。
        f.key(SDL_KEYDOWN,arrow,KMOD_LALT | KMOD_LSHIFT);
        f.mouse(SDL_MOUSEBUTTONDOWN,20,30); f.mouse(SDL_MOUSEMOTION,30,40); f.mouse(SDL_MOUSEBUTTONUP,30,40);
        f.key(SDL_KEYUP,SDL_SCANCODE_LALT);
        f.key(SDL_KEYDOWN,arrow,KMOD_NONE,1); f.key(SDL_KEYDOWN,arrow);
        f.key(SDL_KEYUP,arrow); f.key(SDL_KEYUP,SDL_SCANCODE_LSHIFT); f.key(SDL_KEYUP,SDL_SCANCODE_A);
        f.key(SDL_KEYDOWN,SDL_SCANCODE_B); f.key(SDL_KEYUP,SDL_SCANCODE_B);
        check(!f.pump() && f.presenter.input_generation() == generation+1 && f.touches.size() == 2 &&
                  f.touches[0].down && !f.touches[1].down &&
                  f.touches[0].x == f.touches[1].x && f.touches[0].y == f.touches[1].y,
              "同view镜像抬起最后已交付点，清pending_move和own pointer，Local保留到真实UP");
        f.expect({{4},{4,225},{},{5},{}}, "镜像释放真实设备Shift/A，重复本地箭头不泄漏且后续键完整处理");
        const auto held = f.presenter.display_degrees(0);
        f.key(SDL_KEYDOWN,arrow); check(!f.pump(), "普通箭头在翻转后仍交给设备");
        f.key(SDL_KEYDOWN,arrow,KMOD_LALT | KMOD_LSHIFT,1);
        f.key(SDL_KEYDOWN,arrow,KMOD_LALT | KMOD_LSHIFT);
        check(!f.pump() && f.presenter.display_degrees(0) == held &&
                  f.presenter.input_generation() == generation+1,
              "先归设备的箭头后来加入MOD/Shift或重复DOWN不能抢作mirror");
        f.key(SDL_KEYUP,arrow); check(!f.pump(), "设备箭头实际UP释放一次");
        f.expect({{static_cast<uint16_t>(arrow)},{}}, "普通上下/左右箭头保持实际HID生命周期");
        f.key_for(SDL_KEYDOWN,arrow,KMOD_LALT | KMOD_LSHIFT,0,f.window_id+1000);
        f.key_for(SDL_KEYUP,arrow,KMOD_NONE,0,f.window_id+1000);
        f.window(SDL_WINDOWEVENT_FOCUS_LOST); f.key(SDL_KEYDOWN,arrow,KMOD_LALT | KMOD_LSHIFT);
        check(!f.pump() && f.presenter.display_degrees(0) == held, "其他窗口及失焦输入不执行mirror");
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED); check(!f.pump(), "恢复mirror输入焦点");
        SDL_SetWindowSize(window,160,160); check(!f.pump(), "用户将镜像窗口调为留边尺寸");
        f.mouse(SDL_MOUSEBUTTONDOWN,0,80); f.mouse(SDL_MOUSEBUTTONUP,0,80);
        f.touches.clear(); check(!f.pump() && f.touches.empty(), "H/V后留边仍拒绝触摸，不夹到面板边缘");
    }
    {
        KeyboardFixture f("Presenter pending mirror local UP");
        if (f.window_id) {
            auto *window = SDL_GetWindowFromID(f.window_id);
            check(f.presenter.draw(f.frame) && !f.pump(), "建立特殊模式mirror的合法静止纹理");
            SDL_SetWindowSize(window,128,192); check(!f.pump(), "设置特殊模式前的实际二倍用户尺度");
            check(SDL_SetWindowFullscreen(window,SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && f.presenter.is_fullscreen(),
                  "mirror恢复判据使用实际全屏flag");
            if (f.presenter.is_fullscreen()) {
                const auto generation = f.presenter.input_generation();
                f.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,KMOD_LALT); f.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
                check(!f.pump(), "全屏Right建立延后尺寸基准");
                f.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,KMOD_LALT | KMOD_LSHIFT);
                check(!f.pump() && f.presenter.display_degrees(0) == 270, "全屏H保留最终镜像和Local DOWN");
                check(SDL_SetWindowFullscreen(window,0) == 0, "mirror恢复前实际退出全屏");
                f.window(SDL_WINDOWEVENT_RESTORED); f.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
                check(!f.pump() && f.presenter.input_generation() == generation+3,
                      "恢复仅适配最终一次布局并保留mirror Local UP");
                int w = 0,h = 0; SDL_GetWindowSize(window,&w,&h);
                check(w == 192 && h == 128, "mirror不重置首个pending基准，恢复正确最终二倍尺寸");
                corners(f,{0,3,2,1});
                f.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,KMOD_LALT | KMOD_LSHIFT); f.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
                check(!f.pump() && f.presenter.display_degrees(0) == 90,
                      "恢复保留真实UP后同一镜像键新DOWN继续生效");
            }
        }
        KeyboardFixture stale("Presenter stale mirror validity",64,64);
        if (stale.window_id) {
            auto *window = SDL_GetWindowFromID(stale.window_id);
            auto *renderer = SDL_GetRenderer(window);
            stale.crop = {0,0,64,64,80,80}; stale.frame = colored_frame(80,80,stale.crop,colors,0);
            check(stale.presenter.update_content(stale.crop,0,stale.on_touch(),stale.on_keyboard()) &&
                      stale.presenter.draw(stale.frame,stale.crop) && !stale.pump(),
                  "建立镜像之前同纹理尺寸的有效来源");
            stale.window(SDL_WINDOWEVENT_MINIMIZED); check(!stale.pump(), "最小化暂停mirror纹理上传");
            stale.crop = {8,8,64,64,80,80}; stale.crop.pixel_degrees = 180;
            const Palette changed{kBottomRight,kTopLeft,kBottomLeft,kTopRight};
            stale.frame = colored_frame(80,80,stale.crop,changed,0);
            check(stale.presenter.draw(stale.frame,stale.crop), "最小化同size新crop/pixel变化不能沿用旧上传有效性");
            stale.window(SDL_WINDOWEVENT_RESTORED); stale.window(SDL_WINDOWEVENT_FOCUS_GAINED);
            SDL_SetWindowSize(window,64,64); check(!stale.pump(), "来源清理后保持方形绘制面检测stale重绘");
            SDL_SetRenderDrawColor(renderer,233,5,9,255); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
            for (const SDL_Scancode key : {SDL_SCANCODE_LEFT,SDL_SCANCODE_DOWN}) {
                stale.key(SDL_KEYDOWN,key,KMOD_LALT | KMOD_LSHIFT); stale.key(SDL_KEYUP,key);
                check(!stale.pump() && !stale.presenter.render_failed(), "失效旧纹理无新Frame仍可提交H/V状态");
                std::vector<Uint32> pixels(64*64); const SDL_Rect full{0,0,64,64};
                check(SDL_RenderReadPixels(renderer,&full,SDL_PIXELFORMAT_ARGB8888,pixels.data(),64*4) == 0 &&
                          std::all_of(pixels.begin(),pixels.end(),[](Uint32 pixel){return pixel == 0xffe90509;}),
                      "镜像不把旧texture按新crop/pixel依据重绘，独立marker保持完整");
            }
            check(stale.presenter.draw(stale.frame,stale.crop) && !stale.pump(), "新的合法上传恢复H+V即时重绘能力");
            corners(stale,{3,1,2,0});
        }
        KeyboardFixture blank("Presenter unuploaded mirror validity");
        if (blank.window_id) {
            auto *renderer = SDL_GetRenderer(SDL_GetWindowFromID(blank.window_id));
            SDL_SetRenderDrawColor(renderer,233,5,9,255); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
            for (const SDL_Scancode key : {SDL_SCANCODE_LEFT,SDL_SCANCODE_UP}) {
                blank.key(SDL_KEYDOWN,key,KMOD_LALT | KMOD_LSHIFT); blank.key(SDL_KEYUP,key);
                check(!blank.pump() && !blank.presenter.render_failed(), "未上传纹理仅保存H/V，不读取未初始化像素");
            }
            std::vector<Uint32> pixels(64*96); const SDL_Rect full{0,0,64,96};
            check(SDL_RenderReadPixels(renderer,&full,SDL_PIXELFORMAT_ARGB8888,pixels.data(),64*4) == 0 &&
                      std::all_of(pixels.begin(),pixels.end(),[](Uint32 pixel){return pixel == 0xffe90509;}),
                  "无合法上传时H/V均不覆盖独立绘制面marker");
            check(blank.presenter.draw(blank.frame) && !blank.pump(), "首个合法Frame沿用H+V用户状态");
            corners(blank,{2,3,0,1});
        }
        KeyboardFixture background("Presenter background H and V disabled",100,80,0,KMOD_LALT,true);
        if (background.window_id) {
            const auto generation = background.presenter.input_generation();
            for (const SDL_Scancode key : {SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_UP,SDL_SCANCODE_DOWN}) {
                background.key(SDL_KEYDOWN,key,KMOD_LALT | KMOD_LSHIFT); background.key(SDL_KEYUP,key);
                check(!background.pump() && background.presenter.display_degrees(0) == 0 &&
                          background.presenter.input_generation() == generation,
                      "无视频背景禁用四个mirror快捷键，不制造视频或几何状态");
            }
            background.expect({}, "背景mirror箭头仍归本地，不泄漏设备输入");
        }
    }
    {
        KeyboardFixture configured("Presenter configured mirror",64,96,0,KMOD_RCTRL);
        if (configured.window_id) {
            check(configured.presenter.draw(configured.frame) && !configured.pump(), "建立自定义MOD镜像的真实纹理");
            configured.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,KMOD_RCTRL | KMOD_LSHIFT);
            configured.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
            check(!configured.presenter.pump({}) && configured.presenter.display_degrees(90) == 270,
                  "自定义右Ctrl镜像生效，无设备回调的文件窗口仍可本机变换");
            corners(configured,{1,0,3,2});
        }
        KeyboardFixture altgr("Presenter AltGr shifted arrows");
        if (altgr.window_id) {
            const auto generation = altgr.presenter.input_generation();
            altgr.key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,KMOD_RALT | KMOD_LSHIFT);
            altgr.key(SDL_KEYUP,SDL_SCANCODE_RIGHT);
            check(!altgr.pump() && altgr.presenter.input_generation() == generation,
                  "默认右Alt+Shift+箭头保留设备输入，不误当成显示mirror");
            altgr.expect({{225,230},{79,225,230},{}}, "AltGr和Shift按实际设备修饰键集合完整释放");
        }
        KeyboardFixture retained("Presenter mirror queue retention");
        if (retained.window_id) {
            std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> foreign(
                SDL_CreateWindow("Mirror foreign queue",0,0,32,32,SDL_WINDOW_HIDDEN),SDL_DestroyWindow);
            check(foreign != nullptr, "镜像队列判据建立实际其他窗口");
            if (foreign) {
                const Uint32 other = SDL_GetWindowID(foreign.get());
                check(retained.presenter.draw(retained.frame) && !retained.pump(), "建立mirror队列保留的真实纹理");
                retained.key(SDL_KEYDOWN,SDL_SCANCODE_A); check(!retained.pump(), "mirror过滤前已有设备键");
                retained.reports.clear(); SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
                retained.key(SDL_KEYDOWN,SDL_SCANCODE_DOWN,KMOD_LALT | KMOD_LSHIFT);
                retained.mouse(SDL_MOUSEBUTTONDOWN,20,30); retained.key(SDL_KEYUP,SDL_SCANCODE_DOWN);
                retained.key(SDL_KEYDOWN,SDL_SCANCODE_B);
                retained.key(SDL_KEYDOWN,SDL_SCANCODE_C); retained.key(SDL_KEYUP,SDL_SCANCODE_C);
                retained.key_for(SDL_KEYDOWN,SDL_SCANCODE_X,KMOD_NONE,0,other);
                retained.mouse(SDL_MOUSEBUTTONDOWN,4,5,1,0,other);
                retained.window(SDL_WINDOWEVENT_EXPOSED); retained.window(SDL_WINDOWEVENT_FOCUS_LOST,other);
                SDL_Event quit{}; quit.type = SDL_QUIT; check(SDL_PushEvent(&quit) == 1, "mirror队列判据保留实际QUIT");
                bool kept_key = false, kept_window = false, kept_other_key = false;
                bool kept_other_pointer = false, kept_quit = false, kept_own_pointer = false;
                const auto keyboard = [&](const KeyboardFixture::Report &report) {
                    retained.reports.push_back(report);
                    if (report != KeyboardFixture::Report{5}) return;
                    std::array<SDL_Event,32> queue{};
                    const int count = SDL_PeepEvents(queue.data(),int(queue.size()),SDL_PEEKEVENT,SDL_FIRSTEVENT,SDL_LASTEVENT);
                    for (int i = 0; i < count; ++i) {
                        const auto &event = queue[static_cast<std::size_t>(i)];
                        kept_key |= event.type == SDL_KEYDOWN && event.key.windowID == retained.window_id;
                        kept_window |= event.type == SDL_WINDOWEVENT && event.window.windowID == retained.window_id;
                        kept_other_key |= event.type == SDL_KEYDOWN && event.key.windowID == other;
                        kept_other_pointer |= event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == other;
                        kept_own_pointer |= event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == retained.window_id;
                        kept_quit |= event.type == SDL_QUIT;
                    }
                };
                check(retained.presenter.pump(retained.on_touch(),keyboard) && kept_key && kept_window &&
                          kept_other_key && kept_other_pointer && kept_quit && !kept_own_pointer,
                      "镜像只删本窗口旧pointer，真实queue的KEY/WINDOW/QUIT及other输入继续存在");
                check(retained.touches.empty(), "mirror旧坐标和其他窗口pointer不制造本窗口触摸");
                retained.expect({{},{5},{5,6},{5},{}}, "mirror保留后续设备键事件，正常QUIT仅释放实际新集合");
            }
        }
    }
}

void presenter_background_windows() {
    for (const unsigned variant : {0u, 1u, 2u, 3u}) {
        scrctl::app::Presenter presenter;
        presenter.set_background(17, 43, 71);
        scrctl::app::WindowSpec spec;
        spec.title = "Presenter background dimensions";
        spec.want_readback = true;
        if (variant == 1) {
            spec.want_w = 317; spec.x = 27; spec.y = 49;
            spec.borderless = true; spec.always_on_top = true;
        } else if (variant == 2) {
            spec.want_h = 103;
        } else if (variant == 3) {
            spec.want_w = 317; spec.want_h = 103; spec.fullscreen = true;
        }
        check(presenter.open_background(spec), "实际背景窗口不需要视频帧即可打开");
        const auto id = window_id_from_events(spec.title.c_str());
        auto *window = SDL_GetWindowFromID(id);
        auto *renderer = window ? SDL_GetRenderer(window) : nullptr;
        check(window && renderer, "背景窗口使用真实 SDL window/renderer");
        if (!window || !renderer) continue;
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        check(spec.fullscreen ? presenter.is_fullscreen()
                              : (width == (spec.want_w ? spec.want_w : 256) &&
                                 height == (spec.want_h ? spec.want_h : 256)),
              "无视频窗口默认各维 256 点，显式单维独立覆盖，全屏标志保留");
        check(!(SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE),
              "背景窗口与 scrcpy 无视频模式一致，不设置视频窗口的可缩放标志");
        if (variant == 1) {
            int x = 0, y = 0; SDL_GetWindowPosition(window, &x, &y);
            check(x == spec.x && y == spec.y &&
                      (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) &&
                      (SDL_GetWindowFlags(window) & SDL_WINDOW_ALWAYS_ON_TOP),
                  "背景窗口保留显式位置、边框及置顶选择");
        }
        const auto color_is = [&](Uint32 expected) {
            int w = 0, h = 0;
            if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) return false;
            std::vector<Uint32> pixels(static_cast<std::size_t>(w) * h);
            const SDL_Rect full{0, 0, w, h};
            return SDL_RenderReadPixels(renderer, &full, SDL_PIXELFORMAT_ARGB8888,
                       pixels.data(), w * static_cast<int>(sizeof(Uint32))) == 0 &&
                   std::all_of(pixels.begin(), pixels.end(), [expected](Uint32 p) { return p == expected; });
        };
        check(color_is(0xff112b47), "open_background 初次呈现完整背景，没有伪像素帧");
        if (variant != 0) continue;
        const auto expose = [&](Uint32 window_id) {
            SDL_Event event{}; event.type = SDL_WINDOWEVENT;
            event.window.windowID = window_id; event.window.event = SDL_WINDOWEVENT_EXPOSED;
            check(SDL_PushEvent(&event) == 1, "将暴露事件送入实际 SDL 队列");
        };
        check(SDL_SetRenderDrawColor(renderer, 233, 5, 9, 255) == 0 && SDL_RenderClear(renderer) == 0,
              "改变真实背景绘制面，避免重绘判据依赖初始颜色");
        SDL_RenderPresent(renderer);
        expose(id + 1000);
        check(!presenter.pump({}) && color_is(0xffe90509), "其他窗口暴露事件不重绘本窗口");
        expose(id);
        check(!presenter.pump({}) && color_is(0xff112b47), "本窗口暴露事件沿现有 pump 重新呈现背景");
        SDL_SetWindowSize(window, 291, 117);
        check(!presenter.pump({}) && color_is(0xff112b47),
              "实际 SDL 改变窗口尺寸后，全绘制面边缘仍是背景色");
        int w = 0, h = 0; SDL_GetRendererOutputSize(renderer, &w, &h);
        check(w == 291 && h == 117, "背景 resize 启用真实新绘制面，不依赖视频 draw");
        SDL_SetRenderDrawColor(renderer, 233, 5, 9, 255); SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Event state{}; state.type = SDL_WINDOWEVENT; state.window.windowID = id;
        state.window.event = SDL_WINDOWEVENT_MINIMIZED;
        check(SDL_PushEvent(&state) == 1, "向实际队列发送本窗口最小化状态事件");
        expose(id);
        check(!presenter.pump({}) && color_is(0xffe90509), "背景最小化状态跳过暴露重绘");
        state.window.event = SDL_WINDOWEVENT_RESTORED;
        check(SDL_PushEvent(&state) == 1, "向实际队列发送本窗口恢复状态事件");
        check(!presenter.pump({}) && color_is(0xff112b47), "背景恢复事件无需视频帧即重新呈现");
        ReadbackDirectory output;
        check(output.open(), "创建背景视频操作拒绝的临时目录");
        const auto path = (output.path / "must-not-save.bmp").string();
        const scrctl::app::Crop crop{0, 0, 64, 96, 64, 96};
        const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
        const auto frame = colored_frame(64, 96, crop, colors, 0);
        check(!presenter.draw(frame, path.c_str()) && !presenter.readback(path) &&
                  !presenter.update_content(crop, 90, {}) && !std::filesystem::exists(path) &&
                  color_is(0xff112b47),
              "背景窗口拒绝视频上传、方向更新及视频回读，不改变实际输出或创建文件");
    }
}

void presenter_background_input() {
    for (const bool close_window : {false, true}) {
        KeyboardFixture f("Presenter background input", 160, 112, 0, KMOD_RCTRL, true);
        if (!f.window_id) continue;
        f.presenter.set_debug_input(true);
        check(!f.frame && !f.pump() && f.presenter.ready_for_paste(),
              "背景窗口未创建 fixture 像素帧，仍可处理输入及显式粘贴");
        auto *window = SDL_GetWindowFromID(f.window_id);
        if (!window) continue;
        const auto size_is = [&](int w, int h) {
            int width = 0, height = 0; SDL_GetWindowSize(window, &width, &height);
            return width == w && height == h;
        };
        f.key(SDL_KEYDOWN, SDL_SCANCODE_Q);
        f.key(SDL_KEYUP, SDL_SCANCODE_Q);
        check(!f.pump(), "无视频窗口的普通 Q 不退出，仍是设备按键");
        f.expect({{20}, {}}, "无视频普通 Q 通过同一物理键盘状态机完整按下及释放");
        const auto generation = f.presenter.input_generation();
        f.mouse(SDL_MOUSEBUTTONDOWN, 20, 30, 2);
        f.mouse(SDL_MOUSEMOTION, 50, 60);
        f.mouse(SDL_MOUSEBUTTONUP, 50, 60);
        for (const auto code : {SDL_SCANCODE_G, SDL_SCANCODE_W}) {
            f.key(SDL_KEYDOWN, code, KMOD_RCTRL); f.key(SDL_KEYUP, code);
        }
        check(!f.pump() && f.touches.empty() && f.reports.empty() && size_is(160, 112) &&
                  f.presenter.input_generation() == generation,
              "背景坐标鼠标、双击及 G/W 不制造触摸、几何或视频行为");
        unsigned pastes = 0;
        f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_RCTRL); f.key(SDL_KEYUP, SDL_SCANCODE_V);
        check(!f.presenter.pump(f.on_touch(), f.on_keyboard(), [&] { ++pastes; }) &&
                  pastes == 1 && f.reports.empty(),
              "背景沿现有配置修饰键处理 MOD+V，不注入本地 V");
        const auto fullscreen = f.presenter.is_fullscreen();
        f.key(SDL_KEYDOWN, SDL_SCANCODE_F11); f.key(SDL_KEYUP, SDL_SCANCODE_F11);
        check(!f.pump() && f.presenter.is_fullscreen() != fullscreen && f.reports.empty(),
              "无视频 F11 切换真实 SDL 全屏，键盘不收到 F11");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_RCTRL); f.key(SDL_KEYUP, SDL_SCANCODE_F);
        check(!f.pump() && f.presenter.is_fullscreen() == fullscreen && f.reports.empty(),
              "无视频配置 MOD+F 沿原路径恢复窗口模式");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
        check(!f.pump() && !f.presenter.ready_for_paste(), "无视频设备键按住状态阻止陈旧粘贴");
        f.window(SDL_WINDOWEVENT_FOCUS_LOST);
        check(!f.pump() && !f.presenter.ready_for_paste(), "背景失焦沿同一输入释放路径暂停新输入");
        f.expect({{4}, {}}, "无视频失焦释放已发送设备按键一次");
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED); check(!f.pump(), "背景恢复本窗口键盘焦点");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A); check(!f.pump(), "背景关闭前保持一个真实设备键状态");
        f.window(SDL_WINDOWEVENT_CLOSE, f.window_id + 1000);
        check(!f.pump(), "其他窗口关闭事件不关闭背景窗口");
        if (close_window) f.window(SDL_WINDOWEVENT_CLOSE);
        else f.key(SDL_KEYDOWN, SDL_SCANCODE_Q, KMOD_RCTRL);
        check(f.pump() && !f.presenter.ready_for_paste(), "背景 MOD+Q 或窗口关闭沿同一退出路径");
        f.expect({{4}, {}}, "背景退出释放设备键，且本地 Q 不进入设备");
        f.release(); f.expect({}, "背景退出后的重复清理不重复释放按键");
    }
}

void presenter_physical_keyboard_events() {
    KeyboardFixture f("Presenter physical keyboard regression");
    if (f.window_id == 0) return;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> foreign(
        SDL_CreateWindow("Foreign keyboard event window", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, 64, 96, SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    check(foreign != nullptr, "创建另一真实窗口以检查 keyboard/window 身份过滤");
    if (!foreign) return;
    const Uint32 foreign_id = SDL_GetWindowID(foreign.get());
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_B, KMOD_LSHIFT);
    f.key(SDL_KEYUP, SDL_SCANCODE_B, KMOD_LSHIFT);
    f.key(SDL_KEYUP, SDL_SCANCODE_LSHIFT, KMOD_LSHIFT);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    check(!f.pump(), "普通物理组合键不退出窗口");
    f.expect({{4}, {4, 225}, {4, 5, 225}, {4, 225}, {4}, {}},
             "实际回调先交付修饰键前缀并保留 A，每次抬起保留其它按住状态");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_Q);
    f.key(SDL_KEYUP, SDL_SCANCODE_Q);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_ESCAPE);
    f.key(SDL_KEYUP, SDL_SCANCODE_ESCAPE);
    check(!f.pump(), "普通 Q 和 Esc 按物理键交付，不退出窗口");
    f.expect({{20}, {}, {41}, {}}, "同轮快速普通 Q/Esc 的 DOWN 和 UP 均有序交付");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_RALT, KMOD_LCTRL | KMOD_RALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A, KMOD_LCTRL | KMOD_RALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_A, KMOD_LCTRL | KMOD_RALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_RALT, KMOD_LCTRL);
    f.key(SDL_KEYUP, SDL_SCANCODE_LCTRL);
    f.pump();
    f.expect({{224, 230}, {4, 224, 230}, {224, 230}, {224}, {}},
             "AltGr 的右 Alt 和 Control 按物理键状态交付，不误作本地 MOD");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_LALT, KMOD_LALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_F);
    check(!f.pump() && f.presenter.is_fullscreen(), "首次 MOD+F 执行一次全屏动作");
    f.expect({}, "先松 MOD 后松 F，本地所有权保持到 UP，不把 F 交付设备");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F11);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F11, KMOD_NONE, 1);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F11);
    f.key(SDL_KEYUP, SDL_SCANCODE_F11, KMOD_RSHIFT);
    check(!f.pump() && !f.presenter.is_fullscreen(), "F11 的重复 DOWN 不再次切换全屏");
    f.expect({}, "本地 F11 的 DOWN/UP 不交付设备，也不从陈旧 UP 快照引入 Shift");

    // scrcpy 将所有 F11 组合留在本地；只有无修饰的新 DOWN 才切换全屏。
    // 修饰键快照不能因为一个被消费的 F11 而进入设备状态，UP 也不能清掉 A。
    for (const Uint16 mods : {Uint16{KMOD_LCTRL}, Uint16{KMOD_LSHIFT},
                             Uint16{KMOD_LCTRL | KMOD_RALT}}) {
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
        f.pump();
        f.expect({{4}}, "先交付普通 A，为带修饰 F11 检查保留已有设备键");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_F11, mods);
        check(!f.pump() && !f.presenter.is_fullscreen(),
              "Ctrl、Shift 或 AltGr 加 F11 不切换全屏");
        f.expect({}, "带非快捷键修饰的 F11 不交付 HID，也不引入修饰键快照");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_F11, KMOD_NONE, 1);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_F11);
        check(!f.pump() && !f.presenter.is_fullscreen(),
              "本地 F11 保持所有权，重复或后来无修饰的 DOWN 不切换全屏");
        f.expect({}, "本地 F11 重复 DOWN 不交付设备");
        f.key(SDL_KEYUP, SDL_SCANCODE_F11, mods);
        f.pump();
        f.expect({}, "带修饰 F11 的 UP 不修改原有 A");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
        f.key(SDL_KEYUP, SDL_SCANCODE_A);
        f.key(SDL_KEYUP, SDL_SCANCODE_B);
        f.pump();
        f.expect({{4, 5}, {5}, {}}, "F11 UP 后 A 仍按住，普通 B 与后续抬起有序交付");
    }
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LCTRL, KMOD_LCTRL);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A, KMOD_LCTRL);
    f.pump();
    f.expect({{224}, {4, 224}}, "先按实际 Control 与 A，以检查 F11 UP 不清其它设备键");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F11, KMOD_LCTRL);
    f.key(SDL_KEYUP, SDL_SCANCODE_F11);
    check(!f.pump() && !f.presenter.is_fullscreen(), "带 Control 的本地 F11 不切换全屏");
    f.expect({}, "本地 F11 的陈旧 UP 快照不能释放已交付的 Control 或 A");
    f.key(SDL_KEYUP, SDL_SCANCODE_LCTRL);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.pump();
    f.expect({{4}, {}}, "实际 Control 和 A 的各自 UP 才释放设备状态");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_F);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_LALT, KMOD_LALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_F, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_F);
    check(!f.pump() && !f.presenter.is_fullscreen(), "已归设备的 F 后收到 MOD 重复 DOWN 不切全屏");
    f.expect({{9}, {}}, "旧 F 保持 Device 所有权直到 UP，不重复交付或转成本地键");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.pump();
    f.expect({{4}, {}}, "未分配动作的 MOD+A 仍归本地，下一次普通 A 可正常使用");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    for (const Uint32 id : {foreign_id, Uint32{0}}) {
        f.key_for(SDL_KEYUP, SDL_SCANCODE_A, KMOD_NONE, 0, id);
        f.key_for(SDL_KEYDOWN, SDL_SCANCODE_B, KMOD_RSHIFT, 0, id);
        f.key_for(SDL_KEYDOWN, SDL_SCANCODE_Q, KMOD_LALT, 0, id);
        f.key_for(SDL_KEYDOWN, SDL_SCANCODE_F11, KMOD_NONE, 0, id);
        f.window(SDL_WINDOWEVENT_CLOSE, id);
    }
    check(!f.pump() && !f.presenter.is_fullscreen(), "外部和未知 ID 的键盘/关闭事件不改变本窗口");
    f.expect({{4}}, "外部 UP 不释放 A，外部 DOWN 不引入 B/Shift");
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.pump();
    f.expect({{}}, "只有本窗口 UP 释放原有 A");

    for (const Uint8 state : {SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_HIDDEN,
                             SDL_WINDOWEVENT_MINIMIZED}) {
        f.touches.clear();
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_X, KMOD_LALT);
        f.mouse(SDL_MOUSEBUTTONDOWN, 16, 24);
        f.pump();
        f.expect({{4}}, "失活前同时保留设备 A、本地 X 与触摸状态");
        f.mouse(SDL_MOUSEMOTION, 32, 48);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
        f.window(state);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_C);
        f.mouse(SDL_MOUSEBUTTONDOWN, 48, 72);
        f.pump();
        f.expect({{4, 5}, {}}, "本窗口失活统一发送一次空键盘报告，丢弃尾部 C");
        check(f.touches.size() == 2 && f.touches.front().down && !f.touches.back().down &&
                  f.touches.back().x == 0.25 && f.touches.back().y == 0.25,
              "统一清理仍按最后已交付触点释放，不发送待合并移动");
        f.window(state);
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED, foreign_id);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_C);
        f.pump();
        f.release();
        f.expect({}, "重复失活与显式释放幂等，外部 focus 不恢复输入");
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        f.key(SDL_KEYUP, SDL_SCANCODE_A, KMOD_RSHIFT);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_X);
        f.key(SDL_KEYUP, SDL_SCANCODE_X);
        f.pump();
        f.expect({{27}, {}}, "恢复后旧 UP 不引入修饰键，本地 X 所有权已清理");
    }

    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.pump(); f.expect({{4}}, "只按键盘、没有拖动时保存 A");
    auto rotated = f.crop; rotated.pixel_degrees = 180;
    check(f.presenter.draw(f.frame, rotated), "仅键盘按住时绘制源方向变化");
    f.presenter.release_touch(f.on_touch());
    f.pump(); f.expect({{}}, "legacy release_touch 不吃掉待发键盘释放，转向统一清理 A");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
    f.pump(); f.expect({{5}}, "方向变化后 B 可重新按下");
    auto resized = rotated; resized.display_w = 128; resized.display_h = 192;
    check(f.presenter.draw(f.frame, resized), "仅键盘按住时绘制面板尺寸变化");
    f.pump(); f.expect({{}}, "没有触摸的面板尺寸变化也清除按住键");

    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    const auto larger_frame = colored_frame(80, 112, resized, colors, 0);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.pump(); f.expect({{4}}, "编码源尺寸改变前只按住键盘 A");
    check(f.presenter.draw(larger_frame, resized), "源帧变大但面板尺寸和轴角保持不变");
    f.pump(); f.expect({{}}, "独立于面板轴角的源编码尺寸变化也释放按住键");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
    f.pump(); f.expect({{5}}, "裁剪改变前只按住键盘 B");
    auto recropped = resized; recropped.x = 4; recropped.y = 6;
    recropped.w = 60; recropped.h = 90;
    check(f.presenter.draw(larger_frame, recropped), "源尺寸和面板轴角不变，仅改变裁剪矩形");
    f.pump(); f.expect({{}}, "裁剪矩形 transition 也统一释放键盘");

    auto unknown = recropped; unknown.input_valid = false;
    check(f.presenter.draw(larger_frame, unknown), "切换到触摸几何未知的画面");
    f.pump(); f.expect({}, "一次性几何变化在空键盘状态下不补空报告");
    f.touches.clear();
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.mouse(SDL_MOUSEBUTTONDOWN, 16, 24);
    f.pump(); f.expect({{4}}, "未知触摸坐标下普通物理 A 仍可按住");
    check(f.touches.empty(), "未知几何仍不交付鼠标触摸");
    check(f.presenter.draw(larger_frame, unknown), "继续显示同一未知几何画面");
    f.pump(); f.expect({}, "持续未知几何不会每帧松开键盘");
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.pump(); f.expect({{}}, "未知几何下仍能正常交付物理 A 的 UP");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
    f.pump(); f.expect({{5}}, "几何恢复前按住普通 B");
    check(f.presenter.draw(f.frame, f.crop), "恢复源方向和触摸坐标依据");
    f.pump(); f.expect({{}}, "unknown 到 known 的一次性 transition 释放 B");
    f.release(); f.release(); f.pump();
    f.expect({}, "重复统一释放和 pump 不追加空键盘报告");
}

void presenter_keyboard_quit_and_close() {
    for (const bool close : {false, true}) {
        KeyboardFixture f(close ? "Presenter keyboard close regression" :
                                  "Presenter keyboard quit regression");
        if (f.window_id == 0) continue;
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
        f.mouse(SDL_MOUSEBUTTONDOWN, 16, 24);
        f.pump(); f.expect({{4}}, "退出前键盘 A 与触摸都保持按下");
        f.key(SDL_KEYDOWN, SDL_SCANCODE_B);
        f.mouse(SDL_MOUSEMOTION, 32, 48);
        if (close) {
            f.window(SDL_WINDOWEVENT_CLOSE);
        } else {
            SDL_Event quit{}; quit.type = SDL_QUIT;
            check(SDL_PushEvent(&quit) == 1, "向实际 SDL 队列送入退出事件");
        }
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_C);
        f.key(SDL_KEYUP, SDL_SCANCODE_A, KMOD_RSHIFT);
        f.mouse(SDL_MOUSEBUTTONDOWN, 48, 72);
        check(f.pump(), "QUIT 或本窗口 CLOSE 返回退出请求");
        f.expect({{4, 5}, {}}, "退出前保留 A/B，退出后只发一次空报告、不派发尾部 C/Shift");
        check(f.touches.size() == 2 && !f.touches.back().down &&
                  f.touches.back().x == 0.25 && f.touches.back().y == 0.25,
              "退出统一释放触摸，待合并移动与尾部按下均不再交付");
        f.release(); f.release();
        f.expect({}, "正式调用方退出收尾重复 release_input 不重复释放键盘");
    }
}

void presenter_clipboard_request_context() {
    KeyboardFixture f("Presenter explicit paste context");
    if (!f.window_id) return;
    unsigned requested = 0, eligible = 0;
    uint64_t requested_generation = 0;
    const auto paste = [&] {
        ++requested;
        requested_generation = f.presenter.input_generation();
        if (f.presenter.ready_for_paste()) ++eligible;
    };
    const auto pump = [&] { return f.presenter.pump(f.on_touch(), f.on_keyboard(), paste); };
    check(f.presenter.ready_for_paste(), "新窗口无按住输入，可接受明确粘贴请求");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT, 1);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_V);
    check(!pump() && requested == 1 && eligible == 1, "MOD+V 只在首次按下请求一次粘贴");
    f.expect({}, "本地 MOD+V 与抬起不先给手机发送普通 V");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT | KMOD_LSHIFT);
    f.key(SDL_KEYUP, SDL_SCANCODE_V);
    check(!pump() && requested == 1, "未提供旧式文字注入，不把 MOD+Shift+V 当成普通粘贴");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V);
    f.key(SDL_KEYUP, SDL_SCANCODE_V);
    check(!pump() && requested == 1, "普通 V 不读取本机剪贴板");
    f.expect({{25}, {}}, "普通 V 仍按物理键盘报告转发");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_V);
    check(!pump() && requested == 2 && eligible == 1 && !f.presenter.ready_for_paste(),
          "有普通键按住时拒绝粘贴，避免完整粘贴报告释放它");
    f.expect({{4}}, "被拒粘贴不影响已按住的 A");
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    check(!pump() && f.presenter.ready_for_paste(), "A 松开之后恢复粘贴资格");
    f.expect({{}}, "真实 A 抬起释放设备键");

    f.mouse(SDL_MOUSEBUTTONDOWN, 8, 16);
    check(!pump() && !f.presenter.ready_for_paste(), "拖动期间不向当前控件注入粘贴");
    f.mouse(SDL_MOUSEBUTTONUP, 8, 16);
    check(!pump() && f.presenter.ready_for_paste(), "鼠标抬起恢复粘贴资格");
    const auto previous = f.presenter.input_generation();
    f.window(SDL_WINDOWEVENT_FOCUS_LOST);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
    check(!pump() && requested == 2 && f.presenter.input_generation() != previous &&
              f.presenter.ready_for_paste(),
          "同轮失焦后恢复也使旧作业代次失效，失焦期间不请求粘贴");
    f.key_for(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT, 0, f.window_id + 1);
    check(!pump() && requested == 2, "外部窗口的 MOD+V 不读取本机剪贴板");

    check(f.presenter.draw(f.frame, f.crop), "以真实渲染建立粘贴坐标上下文");
    check(!pump(), "消费首次几何更新");
    const auto geometry_generation = f.presenter.input_generation();
    auto changed = f.crop;
    changed.x = 2;
    changed.w -= 2;
    check(f.presenter.draw(f.frame, changed) && !f.presenter.ready_for_paste(),
          "裁剪变更先暂停粘贴，等待统一输入清理");
    check(!pump() && f.presenter.input_generation() != geometry_generation &&
              f.presenter.ready_for_paste(),
          "几何清理更新代次后才能接收新粘贴");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_V);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_Q, KMOD_LALT);
    check(pump() && requested == 3 && eligible == 2 &&
              requested_generation != f.presenter.input_generation() &&
              !f.presenter.ready_for_paste(),
          "同轮先粘贴再退出，使已请求作业失效且阻止迟到按键");
    const auto empty = f.presenter.input_generation();
    f.release();
    check(f.presenter.input_generation() != empty,
          "无按住输入的显式退出清理也使异步作业失效");
}

void window_action_geometry() {
    using scrctl::app::content_rect;
    scrctl::app::ContentRect content;
    check(content_rect(64, 96, 160, 160, content) && content.x == 27 && content.y == 0 &&
              content.w == 106 && content.h == 160,
          "显式内容矩形与真实左右留边的像素边界一致");
    check(content_rect(96, 64, 160, 160, content) && content.x == 0 && content.y == 27 &&
              content.w == 160 && content.h == 106,
          "旋转后的内容矩形正确居中上下留边");
    check(content_rect(64, 96, 320, 320, content) && content.x == 53 && content.y == 0 &&
              content.w == 213 && content.h == 320,
          "2x 绘制面使用自身像素布局，不把点数当作物理像素");
    check(content_rect(64, 96, 128, 192, content) && content.x == 0 && content.y == 0 &&
              content.w == 128 && content.h == 192,
          "像素 1:1 的绘制面铺满全部源内容");
    check(content_rect(1, std::numeric_limits<int>::max(), 1, 1, content) &&
              content.x == 0 && content.y == 0 && content.w == 1 && content.h == 1,
          "极小绘制面及极端源比例仍使用有界的一像素内容矩形");
    content = {7, 8, 9, 10};
    check(!content_rect(64, 96, 0, 160, content) && content.x == 7 && content.y == 8 &&
              content.w == 9 && content.h == 10,
          "无绘制面时拒绝布局，不发布部分或陈旧矩形");
    using scrctl::app::pixel_perfect_window;
    using scrctl::app::window_without_borders;
    int w = 0, h = 0;
    check(pixel_perfect_window(64, 96, 160, 160, 160, 160, w, h) && w == 64 && h == 96,
          "G 在 1x 绘制面恢复内容像素尺寸");
    check(pixel_perfect_window(1125, 2436, 400, 800, 800, 1600, w, h) && w == 563 && h == 1218,
          "G 在 2x 绘制面按最近整数点换算，奇数像素的半点向上取整");
    check(pixel_perfect_window(96, 64, 400, 400, 600, 600, w, h) && w == 64 && h == 43,
          "G 接受非整数 DPI 比例，不按可用显示区缩小");
    check(pixel_perfect_window(96, 64, 400, 400, 800, 400, w, h) && w == 48 && h == 64,
          "G 的横纵点数分别使用真实绘制面比例");
    check(pixel_perfect_window(1, 1, 1, 1, 4, 4, w, h) && w == 1 && h == 1,
          "G 的最小窗口为一整数点");
    const int max = std::numeric_limits<int>::max();
    w = 7; h = 9;
    check(!pixel_perfect_window(max, max, max, max, 1, 1, w, h) && w == 7 && h == 9,
          "G 拒绝不能表示的点数，且不发布部分尺寸");
    check(!pixel_perfect_window(0, 96, 160, 160, 160, 160, w, h) && w == 7 && h == 9,
          "G 拒绝无效内容尺寸");
    check(window_without_borders(64, 96, 160, 160, 160, 160, w, h) && w == 106 && h == 160,
          "W 的宽留边保持高度，宽度按比例向下取整");
    check(window_without_borders(96, 64, 160, 160, 160, 160, w, h) && w == 160 && h == 106,
          "W 的高留边保持宽度，内容旋转后仍取正确轴");
    check(window_without_borders(64, 96, 160, 160, 320, 320, w, h) && w == 106 && h == 160,
          "W 在 2x DPI 下保持窗口点数中的一维");
    check(window_without_borders(64, 96, 160, 160, 320, 160, w, h) && w == 53 && h == 160,
          "W 按绘制面比例处理横纵 DPI 不同的情况");
    check(window_without_borders(64, 96, 106, 160, 106, 160, w, h) && w == 106 && h == 160,
          "W 对像素取整后已合比例的尺寸幂等，不逐次缩小");
    for (const bool swapped : {false, true}) {
        const int source_w = swapped ? 2436 : 1125, source_h = swapped ? 1125 : 2436;
        const int points_w = swapped ? 1218 : 563, points_h = swapped ? 563 : 1218;
        int current_w = points_w, current_h = points_h;
        for (int repeat = 0; repeat < 4; ++repeat) {
            check(window_without_borders(source_w, source_h, current_w, current_h,
                                          current_w * 2, current_h * 2, w, h) &&
                      w == points_w && h == points_h,
                  "2x 奇数源的 G 窗口连续 W 不交替缩小宽高，旋转后同样幂等");
            current_w = w; current_h = h;
        }
    }
    struct DpiCase { int view_w, view_h, num_w, num_h, denominator, expected_w; };
    for (const auto &dpi : {DpiCase{1125, 2436, 2, 2, 1, 110},
                            DpiCase{64, 96, 3, 3, 2, 160},
                            DpiCase{1125, 2436, 2, 3, 1, 166}}) {
        // 第一轮实际移除宽留边，之后使用已发布点数重新计算绘制面尺寸。
        int current_w = 240, current_h = 240;
        for (int repeat = 0; repeat < 4; ++repeat) {
            check(window_without_borders(dpi.view_w, dpi.view_h, current_w, current_h,
                                          current_w * dpi.num_w / dpi.denominator,
                                          current_h * dpi.num_h / dpi.denominator, w, h) &&
                      w == dpi.expected_w && h == 240,
                  "W 一次去边后按真实 2x、1.5x 或非等轴 DPI 连续执行仍幂等");
            current_w = w; current_h = h;
        }
    }
    check(window_without_borders(64, 96, 106, 160, 212, 320, w, h) && w == 106 && h == 160,
          "2x 的 W 已取整窗口继续 W 不累积小于一点的像素误差");
    check(window_without_borders(64, 96, 108, 160, 216, 320, w, h) && w == 106 && h == 160,
          "2x 仍有超过一窗口点的实际宽留边时 W 继续缩小");
    check(window_without_borders(96, 64, 160, 108, 320, 216, w, h) && w == 160 && h == 106,
          "2x 仍有超过一窗口点的实际高留边时 W 继续缩小");
    check(window_without_borders(max, 1, max, max, max, max, w, h) && w == max && h == 1,
          "W 的极端比例计算不溢出且不扩大原尺寸");
    w = 7; h = 9;
    check(!window_without_borders(64, 96, 160, 160, 0, 160, w, h) && w == 7 && h == 9,
          "W 拒绝无效绘制面尺寸且保持原输出参数");
}

void content_change_geometry() {
    using scrctl::app::window_for_content;
    int w = 0, h = 0;
    // 点数分别来自 2x 奇数源、1.5x 源和已去留边的整数窗口。
    struct Case { int cw, ch, ww, wh; };
    for (const auto &c : {Case{1125, 2436, 563, 1218}, Case{64, 96, 43, 64},
                          Case{64, 96, 106, 160}}) {
        int old_w = c.cw, old_h = c.ch, current_w = c.ww, current_h = c.wh;
        for (int i = 0; i < 12; ++i) {
            const bool swapped = (i % 2) == 0;
            check(window_for_content(old_w, old_h, old_h, old_w, current_w, current_h,
                                      0, 0, w, h) &&
                      w == (swapped ? c.wh : c.ww) && h == (swapped ? c.ww : c.wh),
                  "奇数 2x、1.5x 及去边窗口连续横竖切换不累积缩小");
            std::swap(old_w, old_h);
            current_w = w; current_h = h;
        }
    }
    check(window_for_content(64, 96, 96, 64, 160, 160, 0, 0, w, h) &&
              w == 160 && h == 106,
          "有明显留边的方形窗口转屏时按当前显示尺度去边");
    for (int i = 0; i < 8; ++i) {
        const int old_w = i % 2 == 0 ? 96 : 64, old_h = i % 2 == 0 ? 64 : 96;
        check(window_for_content(old_w, old_h, old_h, old_w, w, h, 0, 0, w, h) &&
                  w == (i % 2 == 0 ? 106 : 160) && h == (i % 2 == 0 ? 160 : 106),
              "首次去边之后重复转屏保持已发布的整点尺寸");
    }
    check(window_for_content(64, 96, 96, 144, 43, 64, 0, 0, w, h) &&
              w == 65 && h == 96 &&
              window_for_content(96, 144, 64, 96, w, h, 0, 0, w, h) && w == 43 && h == 64,
          "内容非轴交换的等比例变化使用有界四舍五入，1.5x 点数往返稳定");
    check(window_for_content(64, 96, 96, 64, 160, 240, 200, 120, w, h) &&
              w == 180 && h == 120,
          "内容变化后的窗口在可用区内按新比例适配");
    check(window_for_content(64, 96, 64, 96, 160, 240, 20, 20, w, h) &&
              w == 160 && h == 240,
          "仅旋转 180 度而有效尺寸相同时不改变用户窗口大小");
    w = 7; h = 9;
    check(!window_for_content(0, 96, 96, 64, 160, 240, 0, 0, w, h) && w == 7 && h == 9,
          "内容适配拒绝无效参数且不发布部分尺寸");
    const int max = std::numeric_limits<int>::max();
    check(!window_for_content(1, 1, max, max, max, max, 0, 0, w, h) && w == 7 && h == 9,
          "内容适配的整数乘除不会溢出，无法表示时保留输出");
}

void presenter_content_updates() {
    KeyboardFixture f("Presenter in-place content update", 80, 120);
    if (!f.window_id) return;
    SDL_Window *window = SDL_GetWindowFromID(f.window_id);
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    check(window && renderer, "内容更新取得原 SDL 窗口和渲染器");
    if (!window || !renderer) return;
    SDL_SetWindowSize(window, 96, 144);
    SDL_SetWindowPosition(window, 100, 120);
    check(!f.pump() && f.presenter.draw(f.frame), "建立用户调整后的窗口大小与位置");
    const auto size_is = [&](int w, int h) {
        int actual_w = 0, actual_h = 0;
        SDL_GetWindowSize(window, &actual_w, &actual_h);
        return actual_w == w && actual_h == h;
    };
    const auto identity_is = [&] {
        int x = 0, y = 0;
        SDL_GetWindowPosition(window, &x, &y);
        return SDL_GetWindowFromID(f.window_id) == window &&
               SDL_GetRenderer(window) == renderer && x == 100 && y == 120;
    };
    const auto update = [&](const scrctl::app::Crop &crop, int degrees) {
        return f.presenter.update_content(crop, degrees, f.on_touch(), f.on_keyboard());
    };
    const auto path = std::filesystem::temp_directory_path() /
        ("scrctl-content-update-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) + ".bmp");
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    for (const int degrees : {90, 180, 270, 0}) {
        const int w = degrees == 90 || degrees == 270 ? 144 : 96;
        const int h = degrees == 90 || degrees == 270 ? 96 : 144;
        check(update(f.crop, degrees) && identity_is() && size_is(w, h),
              "转屏复用原窗口和 renderer，保留用户位置及相对显示尺度");
        check(f.presenter.draw(f.frame, path.string().c_str()),
              "同一 renderer 转屏后实际绘制并保存完整回读");
        Palette expected{};
        for (int i = 0; i < 4; ++i) {
            const int source = (i - degrees / 90 + 4) % 4;
            expected[static_cast<std::size_t>(i)] = colors[static_cast<std::size_t>(source)];
        }
        check_presenter_readback(path.string(), expected, w, h, "原地转屏");
        check(!f.pump(), "转屏后的 SDL 尺寸事件不关闭窗口");
    }
    auto generation = f.presenter.input_generation();
    check(update(f.crop, 180) && size_is(96, 144) && identity_is() &&
              f.presenter.input_generation() == generation + 1,
          "180 度同尺寸更新仅改变显示与输入方向，不重设窗口大小");
    check(f.presenter.draw(f.frame) && !f.pump(), "同尺寸方向更新后绘制和事件继续有效");
    generation = f.presenter.input_generation();
    const auto padded = colored_frame(80, 112, f.crop, colors, 12);
    check(update(f.crop, 180) && f.presenter.input_generation() == generation &&
              f.presenter.draw(padded, f.crop) && size_is(96, 144) && identity_is(),
          "只有编码填充和纹理尺寸变化时不触发窗口布局动作");
    check(!f.pump(), "处理编码尺寸变化的旧输入失效");
    check(update(f.crop, 0) && f.presenter.draw(f.frame) && !f.pump(),
          "恢复竖屏输入依据");
    f.reports.clear(); f.touches.clear();
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.mouse(SDL_MOUSEBUTTONDOWN, 24, 36);
    check(!f.pump() && f.touches.size() == 1 && f.touches.front().down,
          "更新前有真实已交付设备按键和触摸");
    f.mouse(SDL_MOUSEMOTION, 72, 108);
    f.mouse(SDL_MOUSEBUTTONDOWN, 48, 48);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    generation = f.presenter.input_generation();
    check(update(f.crop, 90) && f.presenter.input_generation() == generation + 1,
          "真实几何更新使旧粘贴代次失效一次");
    f.expect({{4}, {}}, "几何更新只释放一次已按住设备键");
    check(f.touches.size() == 2 && !f.touches.back().down &&
              f.touches.front().x == f.touches.back().x &&
              f.touches.front().y == f.touches.back().y,
          "几何更新释放最后已交付位置，不使用尚在队列中的移动");
    int pasted = 0;
    check(!f.presenter.pump(f.on_touch(), f.on_keyboard(), [&] { ++pasted; }) &&
              pasted == 0 && f.touches.size() == 2 && f.reports.empty(),
          "旧 MOUSEBUTTONDOWN、MOD+V 与 UP 不按新几何注入或重启粘贴");
    check(f.presenter.draw(f.frame), "新方向可在同一 renderer 正常绘制");
    f.mouse(SDL_MOUSEBUTTONDOWN, 108, 24);
    f.mouse(SDL_MOUSEBUTTONUP, 108, 24);
    check(!f.pump() && f.touches.size() == 4 &&
              std::abs(f.touches[2].x - 0.25) < 0.02 &&
              std::abs(f.touches[2].y - 0.25) < 0.02,
          "更新后的新鼠标事件按新方向映射到同一设备位置");

    check(update(f.crop, 0) && f.presenter.draw(f.frame) && !f.pump(),
          "准备最小化事件期间的延后内容适配");
    f.window(SDL_WINDOWEVENT_MINIMIZED);
    check(!f.pump(), "真实 SDL 最小化事件暂停输入");
    generation = f.presenter.input_generation();
    check(update(f.crop, 90) && update(f.crop, 180) && update(f.crop, 270) &&
              size_is(96, 144) && identity_is(),
          "特殊模式多次更新只记第一次内容基准，不更改当前窗口模式与大小");
    f.touches.clear(); f.reports.clear();
    f.window(SDL_WINDOWEVENT_RESTORED);
    f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
    f.mouse(SDL_MOUSEBUTTONDOWN, 40, 40);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
    check(!f.presenter.pump(f.on_touch(), f.on_keyboard(), [&] { ++pasted; }) &&
              size_is(144, 96) && identity_is() &&
              f.presenter.input_generation() == generation + 4 &&
              f.touches.empty() && f.reports.empty() && pasted == 0,
          "恢复事件先按最终内容一次适配再清旧点击与粘贴，不泄漏到下一事件");
    generation = f.presenter.input_generation();
    check(!f.pump() && f.presenter.input_generation() == generation,
          "恢复后延后动作已消费，不反复调整大小或失效粘贴");

    // 使用后端真实 fullscreen flags；不靠合成 WINDOWEVENT 冒充模式已切换。
    SDL_SetWindowSize(window, 96, 144);
    check(update(f.crop, 0) && !f.pump(), "恢复全屏测试的普通内容基准");
    SDL_SetWindowSize(window, 96, 144);
    check(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 &&
              f.presenter.is_fullscreen(), "后端实际进入全屏模式");
    if (f.presenter.is_fullscreen()) {
        check(update(f.crop, 90) && update(f.crop, 180) && update(f.crop, 270) &&
                  f.presenter.is_fullscreen() && SDL_GetWindowFromID(f.window_id) == window &&
                  SDL_GetRenderer(window) == renderer,
              "全屏期间内容更新保留同一窗口与后端全屏状态");
        check(SDL_SetWindowFullscreen(window, 0) == 0 && !f.pump() && size_is(144, 96),
              "退出全屏后按最初普通内容基准和最终方向一次适配");
    }

    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> foreign(
        SDL_CreateWindow("Content update foreign", 0, 0, 32, 32, SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    check(foreign != nullptr, "创建另一窗口检查过滤范围");
    if (foreign) {
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        const Uint32 foreign_id = SDL_GetWindowID(foreign.get());
        f.key_for(SDL_KEYDOWN, SDL_SCANCODE_A, KMOD_NONE, 0, foreign_id);
        f.window(SDL_WINDOWEVENT_FOCUS_LOST, foreign_id);
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        SDL_Event quit{};
        quit.type = SDL_QUIT;
        check(SDL_PushEvent(&quit) == 1 && update(f.crop, 0),
              "内容更新前排入其它窗口输入、本窗口生命周期与全局退出");
        std::array<SDL_Event, 16> queued{};
        const int count = SDL_PeepEvents(queued.data(), static_cast<int>(queued.size()),
                                         SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
        bool key = false, other_window = false, own_window = false, has_quit = false;
        for (int i = 0; i < count; ++i) {
            const auto &event = queued[static_cast<std::size_t>(i)];
            key |= event.type == SDL_KEYDOWN && event.key.windowID == foreign_id;
            other_window |= event.type == SDL_WINDOWEVENT && event.window.windowID == foreign_id;
            own_window |= event.type == SDL_WINDOWEVENT && event.window.windowID == f.window_id;
            has_quit |= event.type == SDL_QUIT;
        }
        check(key && other_window && own_window && has_quit,
              "临时过滤仅丢本窗口旧输入，保留其它窗口、WINDOWEVENT 和 QUIT");
        check(f.pump(), "保留的全局 QUIT 仍正常退出");
    }
    std::filesystem::remove(path);
}

void presenter_source_queue_changes() {
    KeyboardFixture f("Presenter same-viewport source queue");
    if (!f.window_id) return;
    const Palette colors{kTopLeft, kTopRight, kBottomRight, kBottomLeft};
    SDL_Window *window = SDL_GetWindowFromID(f.window_id);
    const auto size_is = [&] {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        return w == 64 && h == 96 && SDL_GetWindowFromID(f.window_id) == window;
    };
    for (int change = 0; change < 4; ++change) {
        auto crop = f.crop;
        auto frame = colored_frame(80, 112, crop, colors, 0);
        check(f.presenter.draw(frame, crop) && !f.pump(), "建立同视口来源队列检查的已知输入依据");
        f.reports.clear(); f.touches.clear();
        f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
        f.mouse(SDL_MOUSEBUTTONDOWN, 16, 24);
        check(!f.pump(), "来源几何变化之前已有设备按住状态");
        f.mouse(SDL_MOUSEMOTION, 48, 72);
        f.mouse(SDL_MOUSEBUTTONDOWN, 32, 48);
        f.key(SDL_KEYDOWN, SDL_SCANCODE_V, KMOD_LALT);
        switch (change) {
        case 0: crop.x = 2; crop.y = 2; break;
        case 1: crop.pixel_degrees = 180; break;
        case 2: crop.display_w *= 2; crop.display_h *= 2; break;
        default: crop.input_valid = false; break;
        }
        frame = colored_frame(80, 112, crop, colors, 0);
        const auto generation = f.presenter.input_generation();
        check(f.presenter.update_content(crop, 0, f.on_touch(), f.on_keyboard()) &&
                  f.presenter.draw(frame, crop) && size_is(),
              "同视口 crop、像素方向、分母或有效性改变仅更新来源，不改变布局");
        int pasted = 0;
        check(!f.presenter.pump(f.on_touch(), f.on_keyboard(), [&] { ++pasted; }) &&
                  f.presenter.input_generation() == generation + 1 && pasted == 0 &&
                  f.touches.size() == 2 && f.touches.front().down && !f.touches.back().down &&
                  f.touches.front().x == f.touches.back().x &&
                  f.touches.front().y == f.touches.back().y,
              "draw 的来源失效在处理旧点击或 MOD+V 前先释放一次并过滤队列");
        f.expect({{4}, {}}, "来源失效清理设备键一次，不把旧粘贴 V 发到设备");
    }
}

void presenter_window_actions() {
    KeyboardFixture f("Presenter window actions", 160, 160);
    if (!f.window_id) return;
    SDL_Window *window = SDL_GetWindowFromID(f.window_id);
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    check(window && renderer, "从真实窗口取得 SDL 尺寸及像素输出接口");
    if (!window || !renderer) return;
    check(f.presenter.draw(f.frame) && !f.pump(), "建立窗口动作的实际渲染和输入上下文");
    const auto size_is = [&](int width, int height) {
        int actual_w = 0, actual_h = 0;
        SDL_GetWindowSize(window, &actual_w, &actual_h);
        if (actual_w != width || actual_h != height) {
            int drawable_w = 0, drawable_h = 0;
            SDL_GetRendererOutputSize(renderer, &drawable_w, &drawable_h);
            std::printf("     window %dx%d, drawable %dx%d, expected %dx%d\n",
                        actual_w, actual_h, drawable_w, drawable_h, width, height);
        }
        return actual_w == width && actual_h == height;
    };
    const auto reset_square = [&] {
        SDL_SetWindowSize(window, 160, 160);
        SDL_SetWindowPosition(window, 40, 60);
        check(!f.pump() && f.presenter.draw(f.frame), "恢复方形留边窗口并刷新真实 SDL 渲染器");
        f.reports.clear(); f.touches.clear();
    };
    const auto shortcut = [&](SDL_Scancode code, Uint16 mods = KMOD_LALT) {
        f.key(SDL_KEYDOWN, code, mods);
        f.key(SDL_KEYUP, code, KMOD_NONE);
        return f.pump();
    };
    reset_square();
    int old_x = 0, old_y = 0;
    SDL_GetWindowPosition(window, &old_x, &old_y);
    const auto generation = f.presenter.input_generation();
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.mouse(SDL_MOUSEBUTTONDOWN, 80, 80);
    f.mouse(SDL_MOUSEMOTION, 90, 90);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT);
    check(!f.pump() && size_is(64, 96), "MOD+G 通过实际 SDL 窗口恢复 64x96 点");
    check(f.presenter.input_generation() == generation + 1,
          "一次尺寸动作使已请求粘贴的代次失效一次");
    f.expect({{4}, {}}, "G 在尺寸变化前释放已有设备键，不注入本地 G");
    check(f.touches.size() == 2 && f.touches.front().down && !f.touches.back().down &&
              f.touches.front().x == f.touches.back().x && f.touches.front().y == f.touches.back().y,
          "G 释放最后已交付触点，丢弃同轮未交付移动，不在尾部重新按下");
    int x = 0, y = 0;
    SDL_GetWindowPosition(window, &x, &y);
    check(x == old_x && y == old_y, "G 只改变尺寸，不主动移动窗口");
    check(f.presenter.draw(f.frame), "G 后绘制整幅内容");
    int ow = 0, oh = 0;
    check(SDL_GetRendererOutputSize(renderer, &ow, &oh) == 0 && ow == 64 && oh == 96,
          "1x SDL 绘制面实际恢复内容像素宽高");
    std::vector<Uint32> pixels(64 * 96);
    SDL_RenderSetLogicalSize(renderer, 0, 0);
    const bool read = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                           pixels.data(), 64 * int(sizeof(Uint32))) == 0;
    check(read && close(at(pixels.data(), 64, 6, 6), kTopLeft) &&
              close(at(pixels.data(), 64, 57, 89), kBottomRight),
          "G 的真实像素回读包含源图两端角块，不截掉内容或残留大面积留边");

    // 尺寸动作保留本地 DOWN 归属；测试非 repeat 重复事件，不只依赖 SDL repeat 标志。
    SDL_SetWindowSize(window, 160, 160);
    check(!f.pump(), "处理动作之后的宿主窗口尺寸变化");
    const auto repeated_generation = f.presenter.input_generation();
    f.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT, 1);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT, 0);
    check(!f.pump() && size_is(160, 160) &&
              f.presenter.input_generation() == repeated_generation,
          "G 的 repeat 及非 repeat 重复 DOWN 均不再调整尺寸");
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    f.key(SDL_KEYUP, SDL_SCANCODE_G);
    check(!f.pump(), "真实 UP 清掉先前设备键和本地尺寸动作的归属");
    f.expect({}, "尺寸动作后的旧设备键 UP 不产生多余空报告");

    reset_square();
    SDL_GetWindowPosition(window, &old_x, &old_y);
    check(!shortcut(SDL_SCANCODE_W) && size_is(106, 160),
          "MOD+W 保持高度，实际窗口宽度缩成 106 点");
    SDL_GetWindowPosition(window, &x, &y);
    check(x == old_x + 27 && y == old_y, "W 缩掉留边后维持原窗口内容中心");
    check(!shortcut(SDL_SCANCODE_W) && size_is(106, 160), "再次 W 不因取整继续缩小窗口");
    f.expect({}, "W 全程不进入设备键盘报告");

    reset_square();
    const auto unchanged_generation = f.presenter.input_generation();
    check(!shortcut(SDL_SCANCODE_G, KMOD_LALT | KMOD_LSHIFT) &&
              !shortcut(SDL_SCANCODE_W, KMOD_LALT | KMOD_LSHIFT) && size_is(160, 160) &&
              f.presenter.input_generation() == unchanged_generation,
          "MOD+Shift+G/W 按官方语义不执行尺寸动作");
    f.key_for(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT, 0, f.window_id + 1);
    f.key_for(SDL_KEYDOWN, SDL_SCANCODE_W, KMOD_LALT, 0, 0);
    check(!f.pump() && size_is(160, 160), "其它窗口或无 windowID 的尺寸快捷键不改变本窗口");
    f.expect({}, "带 Shift 和外部窗口的快捷键不泄漏键盘输入");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_G);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_W);
    f.key(SDL_KEYUP, SDL_SCANCODE_G);
    f.key(SDL_KEYUP, SDL_SCANCODE_W);
    check(!f.pump() && size_is(160, 160), "普通 G/W 输入不改变窗口尺寸");
    f.expect({{10}, {10, 26}, {26}, {}}, "普通 G/W 仍保留完整设备键盘状态");

    f.key(SDL_KEYDOWN, SDL_SCANCODE_X, KMOD_LALT);
    check(!f.pump(), "登记仍按住的另一未分配本地组合键");
    check(!shortcut(SDL_SCANCODE_G), "另一本地键按住时执行 G");
    f.key(SDL_KEYUP, SDL_SCANCODE_LALT);
    f.key(SDL_KEYDOWN, SDL_SCANCODE_X);
    f.key(SDL_KEYUP, SDL_SCANCODE_X);
    check(!f.pump(), "尺寸动作之后 MOD 先抬起，再收到另一按住本地键的重复 DOWN");
    f.expect({}, "G 也保留其它本地键的归属，不在 MOD 松开后转给设备");

    reset_square();
    f.mouse(SDL_MOUSEBUTTONDOWN, 80, 80, 2);
    f.mouse(SDL_MOUSEBUTTONUP, 80, 80, 2);
    check(!f.pump() && size_is(160, 160) && f.touches.size() == 2 &&
              f.touches.front().down && !f.touches.back().down,
          "内容区域双击保持正常触摸，不触发去留边动作");
    f.touches.clear();
    const auto border_generation = f.presenter.input_generation();
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 1);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80, 1);
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 2);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80, 2);
    check(!f.pump() && size_is(106, 160) && f.touches.empty(),
          "双击真实宽留边等同 W，第一击及第二击均不发送手机触摸");
    check(f.presenter.input_generation() == border_generation + 1,
          "留边双击只执行一次动作并使旧粘贴失效");

    reset_square();
    f.mouse(SDL_MOUSEBUTTONDOWN, 80, 80);
    f.mouse(SDL_MOUSEMOTION, 90, 90);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80);
    check(!f.pump() && f.touches.size() == 3 && !f.touches.back().down &&
              f.touches[1].x == f.touches.back().x && f.touches[1].y == f.touches.back().y,
          "已有拖动在留边区域抬起时仍释放最后有效触点");
    f.touches.clear();
    f.mouse(SDL_MOUSEBUTTONDOWN, 80, 80);
    f.mouse(SDL_MOUSEMOTION, 90, 90);
    f.mouse(SDL_MOUSEMOTION, 0, 80);
    check(!f.pump() && f.touches.size() == 2 && !f.touches.back().down &&
              f.touches.front().x == f.touches.back().x && f.touches.front().y == f.touches.back().y,
          "拖动进入留边立即释放已交付触点并丢弃未交付移动");
    f.touches.clear();
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80);
    f.mouse(SDL_MOUSEBUTTONDOWN, 159, 80);
    f.mouse(SDL_MOUSEBUTTONUP, 159, 80);
    check(!f.pump() && f.touches.empty(), "左右留边单击均不夹到设备边缘发送触摸");

    f.mouse(SDL_MOUSEBUTTONDOWN, 26, 80);
    f.mouse(SDL_MOUSEBUTTONUP, 26, 80);
    f.mouse(SDL_MOUSEBUTTONDOWN, 133, 80);
    f.mouse(SDL_MOUSEBUTTONUP, 133, 80);
    check(!f.pump() && f.touches.empty(),
          "紧邻内容的左右留边像素不因 SDL 整数截断变成有效触摸");
    f.touches.clear();
    f.mouse(SDL_MOUSEBUTTONDOWN, 27, 80);
    f.mouse(SDL_MOUSEBUTTONUP, 27, 80);
    check(!f.pump() && f.touches.size() == 2 && f.touches.front().down &&
              !f.touches.back().down && f.touches.front().x == 0,
          "与留边相邻的真实内容首列仍可触摸，不粗略拒绝逻辑坐标零");
    f.touches.clear();
    f.mouse(SDL_MOUSEBUTTONDOWN, 26, 80, 1);
    f.mouse(SDL_MOUSEBUTTONUP, 26, 80, 1);
    f.mouse(SDL_MOUSEBUTTONDOWN, 26, 80, 2);
    f.mouse(SDL_MOUSEBUTTONUP, 26, 80, 2);
    check(!f.pump() && f.touches.empty() && size_is(106, 160),
          "紧邻内容的留边双击仍去边，包含第一击且不发手机触摸");
    reset_square();

    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.mouse(SDL_MOUSEBUTTONDOWN, 80, 80);
    f.mouse(SDL_MOUSEMOTION, 90, 90);
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 2);
    check(!f.pump() && size_is(106, 160) && f.touches.size() == 2 &&
              f.touches.front().down && !f.touches.back().down,
          "留边双击也释放已有拖动，并丢弃同轮待发移动");
    f.expect({{4}, {}}, "留边双击在调整窗口前释放设备按键");
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    check(!f.pump(), "留边动作后消费旧按键的 UP");

    KeyboardFixture horizontal("Presenter horizontal borders", 160, 160, 90);
    if (horizontal.window_id) {
        check(horizontal.presenter.draw(horizontal.frame) && !horizontal.pump(),
              "建立上下留边的实际渲染窗口");
        horizontal.mouse(SDL_MOUSEBUTTONDOWN, 80, 0);
        horizontal.mouse(SDL_MOUSEBUTTONUP, 80, 0);
        horizontal.mouse(SDL_MOUSEBUTTONDOWN, 80, 159);
        horizontal.mouse(SDL_MOUSEBUTTONUP, 80, 159);
        check(!horizontal.pump() && horizontal.touches.empty(), "上下留边单击不向设备发送触摸");
        horizontal.mouse(SDL_MOUSEBUTTONDOWN, 80, 0, 2);
        horizontal.mouse(SDL_MOUSEBUTTONUP, 80, 0, 2);
        check(!horizontal.pump() && horizontal.touches.empty(), "双击上留边只执行窗口动作");
        SDL_GetWindowSize(SDL_GetWindowFromID(horizontal.window_id), &ow, &oh);
        check(ow == 160 && oh == 106, "上下留边双击保持宽度，缩小高度");
    }

    reset_square();
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 2, SDL_TOUCH_MOUSEID);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80, 2, SDL_TOUCH_MOUSEID);
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 2, 0, f.window_id + 1);
    check(!f.pump() && size_is(160, 160) && f.touches.empty(),
          "触摸模拟鼠标及其它窗口的留边双击不调整本窗口或注入触摸");
    f.mouse(SDL_MOUSEBUTTONDOWN, 0, 80, 2);
    f.mouse(SDL_MOUSEBUTTONUP, 0, 80, 2);
    check(!f.presenter.pump({}, f.on_keyboard()) && size_is(106, 160),
          "没有设备触摸回调的文件窗口也可双击留边去边");

    // 用真实窗口身份的公开事件驱动应用状态，dummy 不支持 native min flag
    // 时也能覆盖最小化暂停显示、继续消费有效源帧和恢复后绘制的正式路径。
    f.window(SDL_WINDOWEVENT_MINIMIZED);
    check(!f.pump() && f.presenter.draw(f.frame),
          "最小化继续消费有效帧，不将暂不可显示误报成退出错误");
    f.key(SDL_KEYDOWN, SDL_SCANCODE_A);
    f.key(SDL_KEYUP, SDL_SCANCODE_A);
    check(!f.pump(), "最小化后继续处理事件，不要求存在绘制面");
    f.expect({}, "最小化时暂停设备输入");
    f.window(SDL_WINDOWEVENT_RESTORED);
    f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
    check(!f.pump() && f.presenter.draw(f.frame), "恢复后的下一有效帧重新绘制");

    SDL_SetWindowSize(window, 240, 240);
    f.touches.clear();
    f.mouse(SDL_MOUSEBUTTONDOWN, 39, 120);
    f.mouse(SDL_MOUSEBUTTONUP, 39, 120);
    f.mouse(SDL_MOUSEBUTTONDOWN, 200, 120);
    f.mouse(SDL_MOUSEBUTTONUP, 200, 120);
    check(!f.pump() && f.touches.empty(),
          "窗口放大尚未绘制新帧时，输入已按新绘制面拒绝两侧紧邻留边");
    f.mouse(SDL_MOUSEBUTTONDOWN, 40, 120);
    f.mouse(SDL_MOUSEBUTTONUP, 40, 120);
    f.mouse(SDL_MOUSEBUTTONDOWN, 120, 120);
    f.mouse(SDL_MOUSEBUTTONUP, 120, 120);
    check(!f.pump() && f.touches.size() == 4 && f.touches[0].x == 0 &&
              f.touches[0].y == .5 && f.touches[2].x == .5 && f.touches[2].y == .5,
          "窗口放大尚未绘制新帧时，内容首列和中心仍映射到正确设备位置");
    check(!f.pump() && f.presenter.draw(f.frame), "软件窗口放大后的第一帧使用新绘制面");
    SDL_Rect full_viewport{};
    SDL_RenderGetViewport(renderer, &full_viewport);
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ow != 240 || oh != 240 || full_viewport.w != ow || full_viewport.h != oh) {
        std::printf("     grown output %dx%d, viewport %d,%d %dx%d\n", ow, oh,
                    full_viewport.x, full_viewport.y, full_viewport.w, full_viewport.h);
    }
    check(ow == 240 && oh == 240 && full_viewport.x == 0 && full_viewport.y == 0 &&
              full_viewport.w == ow && full_viewport.h == oh,
          "软件绘制面放大后保持完整 output viewport，不残留旧的小裁剪区");
    std::vector<Uint32> grown(240 * 240);
    const bool grown_read = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                                  grown.data(), 240 * int(sizeof(Uint32))) == 0;
    check(grown_read && close(at(grown.data(), 240, 185, 225), kBottomRight),
          "窗口放大后第一帧的远侧角块仍有真实像素，不被旧 surface 裁掉");

    reset_square();
    check(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0,
          "用实际 SDL API 切入全屏状态");
    check(!f.pump(), "消费全屏窗口事件");
    SDL_GetWindowSize(window, &ow, &oh);
    const auto full_generation = f.presenter.input_generation();
    check(!shortcut(SDL_SCANCODE_G) && !shortcut(SDL_SCANCODE_W) && size_is(ow, oh) &&
              f.presenter.input_generation() == full_generation,
          "全屏模式的 G/W 不改变窗口尺寸或输入代次");
    check(SDL_SetWindowFullscreen(window, 0) == 0 && !f.pump(), "恢复窗口模式");
    reset_square();
    for (const Uint32 flag : {Uint32(SDL_WINDOW_MAXIMIZED), Uint32(SDL_WINDOW_MINIMIZED)}) {
        if (flag == SDL_WINDOW_MAXIMIZED) SDL_MaximizeWindow(window);
        else SDL_MinimizeWindow(window);
        if ((SDL_GetWindowFlags(window) & flag) == 0) {
            // dummy 不实现最大化/最小化；不伪造 flag 或把无效状态当作已验证。
            std::printf("  SKIP 当前 SDL 驱动不支持窗口状态 %u\n", unsigned(flag));
            continue;
        }
        check(true, "实际 SDL 窗口进入最大化或最小化状态");
        check(!f.pump(), "消费最大化或最小化窗口事件");
        // 单独打开输入门控，让拒绝依据来自窗口 flag，避免最小化分支只测到失焦。
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        check(!f.pump(), "单独开启输入门控以检查窗口状态限制");
        SDL_GetWindowSize(window, &ow, &oh);
        const auto state_generation = f.presenter.input_generation();
        check(!shortcut(SDL_SCANCODE_G) && !shortcut(SDL_SCANCODE_W) && size_is(ow, oh) &&
                  f.presenter.input_generation() == state_generation,
              "最大化或最小化模式的 G/W 不执行尺寸或输入清理动作");
        SDL_RestoreWindow(window);
        f.window(SDL_WINDOWEVENT_FOCUS_GAINED);
        check(!f.pump(), "恢复正常窗口和输入状态");
    }

    KeyboardFixture rotated("Presenter rotated pixel size", 160, 160, 90);
    check(rotated.window_id && rotated.presenter.draw(rotated.frame) && !rotated.pump(),
          "建立旋转后的真实窗口");
    if (rotated.window_id) {
        rotated.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT);
        rotated.key(SDL_KEYUP, SDL_SCANCODE_G);
        check(!rotated.pump(), "旋转窗口执行像素尺寸动作");
        SDL_GetWindowSize(SDL_GetWindowFromID(rotated.window_id), &ow, &oh);
        check(ow == 96 && oh == 64, "G 采用旋转后内容尺寸，不使用编码帧原来的宽高");
    }
    KeyboardFixture configured("Presenter configured size shortcuts", 160, 160, 0, KMOD_RCTRL);
    if (configured.window_id) {
        configured.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_LALT);
        configured.key(SDL_KEYUP, SDL_SCANCODE_G);
        check(!configured.pump(), "默认修饰键不能触发自定义 G 快捷键");
        SDL_GetWindowSize(SDL_GetWindowFromID(configured.window_id), &ow, &oh);
        check(ow == 160 && oh == 160, "自定义 shortcut-mod 替换默认尺寸动作修饰键");
        configured.key(SDL_KEYDOWN, SDL_SCANCODE_G, KMOD_RCTRL);
        configured.key(SDL_KEYUP, SDL_SCANCODE_G);
        check(!configured.pump(), "自定义修饰键触发 G 快捷键");
        SDL_GetWindowSize(SDL_GetWindowFromID(configured.window_id), &ow, &oh);
        check(ow == 64 && oh == 96, "自定义 MOD+G 按同一像素规则调整尺寸");
    }
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
    presenter_physical_keyboard_events();
    presenter_keyboard_quit_and_close();
    presenter_clipboard_request_context();
    window_action_geometry();
    content_change_geometry();
    presenter_content_updates();
    presenter_source_queue_changes();
    presenter_window_actions();
    presenter_display_rotation();
    presenter_display_flips();
    presenter_background_windows();
    presenter_background_input();

    SDL_Quit();
    if (failures != 0) {
        std::printf("render_test: %d 项失败\n", failures);
        return 1;
    }
    std::printf("render_test: 全部通过\n");
    return 0;
}
