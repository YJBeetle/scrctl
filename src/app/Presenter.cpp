#include "i18n/Translation.h"
#include "app/Presenter.h"

#include "app/RenderPanel.h"
#include <algorithm>
#include <cstdio>

namespace scrctl::app {

void Presenter::set_background(uint8_t r, uint8_t g, uint8_t b) {
    bg_[0] = r;
    bg_[1] = g;
    bg_[2] = b;
}

bool Presenter::open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
                     bool scale_given, const WindowSpec &spec) {
    const std::string &title = spec.title;
    const bool want_readback = spec.want_readback;
    const int want_w = spec.want_w;
    const int want_h = spec.want_h;
    src_ = crop;
    degrees_ = degrees;
    scrctl::app::viewport_size(crop, degrees_, view_w_, view_h_);
    if (want_w > 0 && want_h > 0) {
        win_w_ = want_w;
        win_h_ = want_h;
    } else {
        SDL_Rect desk{};
        if (SDL_GetDisplayBounds(0, &desk) != 0 || desk.w <= 0) {
            desk.w = win_w_fallback;
            desk.h = win_h_fallback;
        }
        // 为标题栏预留空间，避免窗口边缘超出屏幕。
        scrctl::app::fit_window(view_w_, view_h_, desk.w, desk.h - 60, scale, scale_given, win_w_,
                                win_h_);
        if (!scale_given && win_w_ < view_w_) {
            std::printf(SCRCTL_TR("Screen %dx%d points; window scaled to %dx%d (override with --scale)\n"), desk.w, desk.h,
                        win_w_, win_h_);
        }
    }

    Uint32 win_flags = SDL_WINDOW_ALLOW_HIGHDPI;
    // 创建窗口时传入全屏与无边框标志，避免先创建普通窗口再切全屏时保留旧尺寸
    // 约束，导致旋转后的画面不能铺满窗口。
    if (!spec.fullscreen) {
        win_flags |= SDL_WINDOW_RESIZABLE;
    }
    if (spec.borderless) {
        win_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (spec.fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    window_ = SDL_CreateWindow(title.c_str(), spec.x, spec.y, win_w_, win_h_, win_flags);
    if (window_ == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create window: %s\n"), SDL_GetError());
        return false;
    }
    if (spec.always_on_top) {
        SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
    }
    // 需要回读时使用软件渲染器；当前 SDL2 Metal 路径不支持可靠的 RenderReadPixels。
    const Uint32 flags = want_readback ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED;
    renderer_ = SDL_CreateRenderer(window_, -1, flags);
    if (renderer_ == nullptr && flags != SDL_RENDERER_SOFTWARE) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create renderer: %s\n"), SDL_GetError());
        return false;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer_, &info) == 0) {
        std::printf(SCRCTL_TR("Render driver: %s\n"), info.name);
    }
    // 设置 logical size 后由 SDL 处理 Retina 比例和窗口缩放；否则以窗口点数
    // 绘制到高 DPI 像素面，会只覆盖部分区域。
    //
    // logical size 使用旋转后的视口尺寸（90/270 度交换宽高），鼠标事件也进入
    // 同一逻辑空间，确保触摸逆变换使用正确的尺寸。
    SDL_RenderSetLogicalSize(renderer_, view_w_, view_h_);
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
    // 纹理采用完整源帧尺寸，裁剪和缩放通过 RenderCopy 的 src/dst 矩形处理。
    // BGRA 内存对应小端 ARGB8888。
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                 frame_w, frame_h);
    if (texture_ == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create texture: %s\n"), SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(texture_, SDL_ScaleModeBest);
    std::printf(SCRCTL_TR(
        "Window %dx%d points / drawable %dx%d pixels / viewport %dx%d (source %dx%d, "
        "crop %dx%d+%d+%d, clockwise rotation %d degrees)\n"),
                win_w_, win_h_, out_w, out_h, view_w_, view_h_, frame_w, frame_h, crop.w, crop.h,
                crop.x, crop.y, degrees_);
    return true;
}

void Presenter::draw(const scrctl::Frame &f, const char *readback_path) {
    // SDL_RenderClear 清除整个目标面，包括等比缩放后的留边区域。
    SDL_SetRenderDrawColor(renderer_, bg_[0], bg_[1], bg_[2], 255);
    SDL_RenderClear(renderer_);
    if (SDL_UpdateTexture(texture_, nullptr, f.pixels.data(), static_cast<int>(f.row_pitch)) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to upload texture: %s\n"), SDL_GetError());
    }
    // 渲染使用逻辑坐标，由 SDL 处理 Retina 缩放和留边。旋转由 draw_rotated
    // 完成，该函数同时用于离线回读测试。
    scrctl::app::draw_rotated(renderer_, texture_, src_, degrees_);
    // 在 Present 之前回读后缓冲；交换缓冲后不能依赖其内容仍有效。
    if (readback_path != nullptr) {
        readback(readback_path);
    }
    SDL_RenderPresent(renderer_);
}

bool Presenter::readback(const std::string &path) {
    int out_w = 0, out_h = 0;
    if (SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0 || out_w <= 0 || out_h <= 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to query drawable dimensions: %s\n"), SDL_GetError());
        return false;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create readback buffer: %s\n"), SDL_GetError());
        return false;
    }
    if (SDL_LockSurface(s) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to lock readback buffer: %s\n"), SDL_GetError());
        SDL_FreeSurface(s);
        return false;
    }
    // 回读前暂时移除 logical size，使用完整输出面的像素坐标。保留逻辑尺寸
    // 时，SDL 会把读区按内容视口转换，窗口有留边时会读到偏移的局部区域。
    SDL_RenderSetLogicalSize(renderer_, 0, 0);
    // 显式指定完整像素矩形；当前 SDL2 software 驱动在 rect=NULL 时曾发生段错误。
    const SDL_Rect full{0, 0, out_w, out_h};
    const int rc =
        SDL_RenderReadPixels(renderer_, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_RenderSetLogicalSize(renderer_, view_w_, view_h_);
    SDL_UnlockSurface(s);
    if (rc != 0) {
        std::fprintf(stderr, SCRCTL_TR("Readback failed: %s\n"), SDL_GetError());
        SDL_FreeSurface(s);
        return false;
    }
    const int save = SDL_SaveBMP(s, path.c_str());
    SDL_FreeSurface(s);
    if (save != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to save image: %s\n"), SDL_GetError());
        return false;
    }
    std::printf(SCRCTL_TR("Window readback saved to %s (%dx%d pixels)\n"), path.c_str(), out_w, out_h);
    return true;
}

void Presenter::report_input(int raw_x, int raw_y, double fx, double fy, const char *tag) const {
    int pw = 0, ph = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(window_, &pw, &ph);
    SDL_GetRendererOutputSize(renderer_, &ow, &oh);
    std::fprintf(stderr,
                 SCRCTL_TR(
                     "[input] %s raw(%d,%d) viewport %d x%d (rotation %d degrees) / window %d x%d / "
                     "drawable %d x%d -> (%.3f, %.3f)\n"),
                 tag, raw_x, raw_y, view_w_, view_h_, degrees_, pw, ph, ow, oh, fx, fy);
}

bool Presenter::pump(const std::function<void(double, double, bool)> &on_touch) {
    SDL_Event e;
    bool quit = false;
    // 合并同一轮的鼠标移动事件，仅发送最后一个位置，减少高采样率鼠标带来的
    // 重复 HID 报告。按下和抬起仍保留各自事件。
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
                if (debug_input_) {
                    report_input(e.button.x, e.button.y, px, py, SCRCTL_TR("down"));
                }
                on_touch(px, py, true);
            }
            break;
        case SDL_MOUSEMOTION:
            if (dragging_ && on_touch) {
                to_display(e.motion.x, e.motion.y, px, py);
                if (debug_input_) {
                    report_input(e.motion.x, e.motion.y, px, py, SCRCTL_TR("move"));
                }
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

Presenter::~Presenter() {
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

void Presenter::to_display(int raw_x, int raw_y, double &fx, double &fy) const {
    scrctl::app::viewport_fraction_to_panel(raw_x, raw_y, src_, degrees_, fx, fy);
}

} // namespace scrctl::app
