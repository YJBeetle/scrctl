#include "i18n/Translation.h"
#include "app/Presenter.h"

#include "app/RenderPanel.h"
#include "hid/Hid.h"
#include <algorithm>
#include <cstdio>
#include <limits>

namespace scrctl::app {
namespace {
// SDL2 的文本输入状态属于整个 SDL 进程。所有 Presenter 都在 SDL 主线程
// 创建/销毁；多窗口共享停用范围，最后一个窗口才恢复调用者原来的状态。
unsigned physical_keyboard_windows = 0;
bool restore_text_input = false;
bool same_crop(const Crop &a, const Crop &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
        a.display_w == b.display_w && a.display_h == b.display_h &&
        a.pixel_degrees == b.pixel_degrees && a.input_valid == b.input_valid;
}
} // namespace

void Presenter::begin_physical_keyboard_mode() {
    if (physical_keyboard_mode_) return;
    if (physical_keyboard_windows++ == 0) {
        restore_text_input = SDL_IsTextInputActive() == SDL_TRUE;
    }
    // 仅忽略 TEXTINPUT 不够：Windows IME 可在生成 SDL_KEYDOWN 前消费物理键。
    // 本前端只转发物理 usages，Unicode 输入由显式剪贴板操作交付。
    SDL_StopTextInput();
    physical_keyboard_mode_ = true;
}

void Presenter::set_background(uint8_t r, uint8_t g, uint8_t b) {
    bg_[0] = r;
    bg_[1] = g;
    bg_[2] = b;
}

bool Presenter::open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
                     bool scale_given, const WindowSpec &spec) {
    if (window_ != nullptr || renderer_ != nullptr) return false;
    video_playback_ = true;
    const int want_w = spec.want_w;
    const int want_h = spec.want_h;
    src_ = crop;
    degrees_ = degrees;
    source_degrees_ = degrees;
    horizontal_flip_ = spec.horizontal_flip;
    scrctl::app::viewport_size(crop, degrees_, view_w_, view_h_);
    SDL_Rect desk{};
    if (want_w == 0 && want_h == 0) {
        if (SDL_GetDisplayBounds(0, &desk) != 0 || desk.w <= 0 || desk.h <= 0) {
            desk.w = win_w_fallback;
            desk.h = win_h_fallback;
        }
    }
    // 为标题栏预留空间；指定单维或双维尺寸时由 fit_window 保留用户的选择。
    if (!scrctl::app::fit_window(view_w_, view_h_, desk.w, desk.h - 60, scale, scale_given,
                                 win_w_, win_h_, want_w, want_h)) {
        std::fprintf(stderr, SCRCTL_TR(
            "Window dimensions are too large; reduce --scale or the requested window size\n"));
        return false;
    }
    if (want_w == 0 && want_h == 0 && !scale_given && win_w_ < view_w_) {
        std::printf(SCRCTL_TR("Screen %dx%d points; window scaled to %dx%d (override with --scale)\n"), desk.w, desk.h,
                    win_w_, win_h_);
    }

    if (!open_window(spec)) return false;
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
    // 纹理采用完整源帧尺寸，裁剪和缩放通过 RenderCopy 的 src/dst 矩形处理。
    // BGRA 内存对应小端 ARGB8888。
    if (!ensure_texture(frame_w, frame_h)) {
        return false;
    }
    std::printf(SCRCTL_TR(
        "Window %dx%d points / drawable %dx%d pixels / viewport %dx%d (source %dx%d, "
        "crop %dx%d+%d+%d, clockwise rotation %d degrees)\n"),
                win_w_, win_h_, out_w, out_h, view_w_, view_h_, frame_w, frame_h, crop.w, crop.h,
                crop.x, crop.y, degrees_);
    if (horizontal_flip_) {
        std::printf(SCRCTL_TR("Display is horizontally flipped before rotation\n"));
    }
    begin_physical_keyboard_mode();
    return true;
}

bool Presenter::open_background(const WindowSpec &spec) {
    if (window_ != nullptr || renderer_ != nullptr || spec.want_w < 0 || spec.want_h < 0) return false;
    video_playback_ = false;
    // scrcpy v5 的无视频窗口各维默认 256 点，仅显式指定的维度被覆盖。
    win_w_ = spec.want_w != 0 ? spec.want_w : 256;
    win_h_ = spec.want_h != 0 ? spec.want_h : 256;
    if (!open_window(spec) || !draw_background()) return false;
    int points_w = 0, points_h = 0, out_w = 0, out_h = 0;
    SDL_GetWindowSize(window_, &points_w, &points_h);
    SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
    std::printf(SCRCTL_TR("Background window %dx%d points / drawable %dx%d pixels\n"),
                points_w, points_h, out_w, out_h);
    begin_physical_keyboard_mode();
    return true;
}

bool Presenter::open_window(const WindowSpec &spec) {
    shortcut_mods_ = spec.shortcut_mods;
    keyboard_ = KeyboardState(shortcut_mods_);
    Uint32 win_flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (video_playback_) win_flags |= SDL_WINDOW_RESIZABLE;
    if (spec.always_on_top) win_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    // 创建窗口时传入全屏与无边框标志，避免先创建普通窗口再切全屏时保留旧尺寸
    // 约束，导致旋转后的画面不能铺满窗口。
    if (spec.borderless) {
        win_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (spec.fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    window_ = SDL_CreateWindow(spec.title.c_str(), spec.x, spec.y, win_w_, win_h_, win_flags);
    if (window_ == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create window: %s\n"), SDL_GetError());
        return false;
    }
    input_active_ = true;
    if (spec.always_on_top) {
        SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
    }
    // 需要回读时使用软件渲染器；当前 SDL2 Metal 路径不支持可靠的 RenderReadPixels。
    const Uint32 flags = spec.want_readback ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED;
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
    // 不启用 SDL 的 logical size，保留队列中原始窗口点坐标。
    // 显示使用完整绘制面中的显式内容矩形，输入通过同一矩形去掉留边。
    if (SDL_RenderSetLogicalSize(renderer_, 0, 0) != 0 || !prepare_output()) {
        std::fprintf(stderr, SCRCTL_TR("Failed to configure viewport: %s\n"), SDL_GetError());
        return false;
    }
    return true;
}

bool Presenter::draw_background() {
    if (!window_ || !renderer_) return false;
    if (minimized_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return true;
    int out_w = 0, out_h = 0;
    if (!prepare_output() || SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to configure viewport: %s\n"), SDL_GetError());
        return false;
    }
    if (out_w <= 0 || out_h <= 0) return true;
    if (SDL_SetRenderDrawColor(renderer_, bg_[0], bg_[1], bg_[2], 255) != 0 ||
        SDL_RenderClear(renderer_) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to render background: %s\n"), SDL_GetError());
        return false;
    }
    SDL_RenderPresent(renderer_);
    return true;
}

bool Presenter::draw(const scrctl::Frame &f, const char *readback_path) {
    return draw(f, src_, readback_path);
}

bool Presenter::ensure_texture(int width, int height) {
    if (texture_ != nullptr && texture_w_ == width && texture_h_ == height) {
        return true;
    }
    SDL_Texture *candidate = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING, width, height);
    if (candidate == nullptr) {
        std::fprintf(stderr, SCRCTL_TR("Failed to create texture: %s\n"), SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(candidate, SDL_ScaleModeBest);
    SDL_DestroyTexture(texture_);
    texture_ = candidate;
    texture_uploaded_ = false;
    texture_w_ = width;
    texture_h_ = height;
    return true;
}

bool Presenter::draw(const scrctl::Frame &f, const Crop &crop, const char *readback_path) {
    if (!video_playback_) return false;
    // 上传整帧时 SDL 按纹理的宽高读取像素。截图不含 HEVC 对齐填充，因此不能
    // 将其较短的行跨度及缓冲用于首次视频帧的较大纹理。
    const uint64_t row_bytes = uint64_t(f.width) * 4;
    if (f.width == 0 || f.height == 0 || f.width > std::numeric_limits<int>::max() ||
        f.height > std::numeric_limits<int>::max() || f.bytes_per_pixel != 4 ||
        f.row_pitch < row_bytes || f.row_pitch > std::numeric_limits<int>::max()) {
        std::fprintf(stderr, SCRCTL_TR("Cannot render frame: invalid dimensions or pixel buffer\n"));
        return false;
    }
    // 此时宽高及 pitch 都已限制为正 int，下面的乘加不会超出 uint64_t。
    const uint64_t needed = uint64_t(f.height - 1) * f.row_pitch + row_bytes;
    if (f.pixels.size() < needed || crop.x < 0 || crop.y < 0 || crop.w <= 0 || crop.h <= 0 ||
        uint64_t(crop.x) + crop.w > f.width || uint64_t(crop.y) + crop.h > f.height ||
        crop.display_w <= 0 || crop.display_h <= 0) {
        std::fprintf(stderr, SCRCTL_TR("Cannot render frame: invalid dimensions or pixel buffer\n"));
        return false;
    }
    if (display_paused_) {
        // 覆盖同一缓冲，不把采集队列堆积成恢复时的历史回放。
        latest_frame_ = f;
        latest_crop_ = crop;
        latest_base_degrees_ = source_degrees_;
        if (!same_crop(crop, src_) || texture_w_ != static_cast<int>(f.width) ||
            texture_h_ != static_cast<int>(f.height) ||
            display_degrees(source_degrees_) != degrees_) paused_input_stale_ = true;
        return draw_uploaded(readback_path);
    }
    const bool source_size_changed = texture_w_ != static_cast<int>(f.width) ||
                                     texture_h_ != static_cast<int>(f.height);
    if (!ensure_texture(static_cast<int>(f.width), static_cast<int>(f.height))) {
        return false;
    }
    // 源坐标依据改变时保留统一释放请求，不依赖是否正在拖动：只按着键盘时
    // 转屏也需要清理。持续未知的同一几何不会每帧重复清除物理键盘状态。
    if (source_size_changed || crop.x != src_.x || crop.y != src_.y ||
        crop.w != src_.w || crop.h != src_.h ||
        crop.input_valid != src_.input_valid || crop.pixel_degrees != src_.pixel_degrees ||
        crop.display_w != src_.display_w || crop.display_h != src_.display_h) {
        release_pending_ = true;
        texture_uploaded_ = false;
    }
    src_ = crop;
    int view_w = 0, view_h = 0;
    viewport_size(src_, degrees_, view_w, view_h);
    view_w_ = view_w;
    view_h_ = view_h;
    if ((minimized_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) &&
        readback_path == nullptr) return true;
    int output_w = 0, output_h = 0;
    if (SDL_GetRendererOutputSize(renderer_, &output_w, &output_h) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to query drawable dimensions: %s\n"), SDL_GetError());
        return false;
    }
    if ((output_w <= 0 || output_h <= 0) && readback_path == nullptr) return true;
    if (SDL_UpdateTexture(texture_, nullptr, f.pixels.data(), static_cast<int>(f.row_pitch)) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to upload texture: %s\n"), SDL_GetError());
        return false;
    }
    texture_uploaded_ = true;
    return draw_uploaded(readback_path);
}

bool Presenter::draw_uploaded(const char *readback_path) {
    if (!video_playback_ || !window_ || !renderer_) return false;
    // open/重建纹理不等于已上传有效像素；重绘使用当前 GPU 纹理。
    if (!texture_uploaded_) return true;
    if ((minimized_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) &&
        readback_path == nullptr) return true;
    int output_w = 0, output_h = 0;
    ContentRect content;
    if (!prepare_output() || SDL_GetRendererOutputSize(renderer_, &output_w, &output_h) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to configure viewport: %s\n"), SDL_GetError());
        return false;
    }
    if ((output_w <= 0 || output_h <= 0) && readback_path == nullptr) return true;
    if (!content_rect(view_w_, view_h_, output_w, output_h, content)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to query drawable dimensions: %s\n"), SDL_GetError());
        return false;
    }
    // SDL_RenderClear 清除整个目标面，包括等比缩放后的留边区域。
    if (SDL_SetRenderDrawColor(renderer_, bg_[0], bg_[1], bg_[2], 255) != 0 ||
        SDL_RenderClear(renderer_) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to render frame: %s\n"), SDL_GetError());
        return false;
    }
    // 目的矩形使用绘制面像素；SDL 完成纹理缩放、翻转和旋转。
    if (!scrctl::app::draw_rotated(renderer_, texture_, src_, degrees_, horizontal_flip_, &content)) {
        std::fprintf(stderr, SCRCTL_TR("Failed to render frame: %s\n"), SDL_GetError());
        return false;
    }
    // 在 Present 之前回读后缓冲；交换缓冲后不能依赖其内容仍有效。
    if (readback_path != nullptr && !readback(readback_path)) {
        return false;
    }
    SDL_RenderPresent(renderer_);
    return true;
}

bool Presenter::refresh_display(bool resume,
                                const std::function<void(double, double, bool)> &on_touch,
                                const KeyboardHandler &on_keyboard,
                                const ButtonHandler &on_button) {
    if (display_paused_ && latest_frame_) {
        // 本地动作保留快捷键 DOWN 的所有权，避免刷新后重复 DOWN 泄漏到设备。
        const bool geometry_changed = paused_input_stale_ || !same_crop(latest_crop_, src_) ||
            display_degrees(latest_base_degrees_) != degrees_ ||
            texture_w_ != static_cast<int>(latest_frame_.width) ||
            texture_h_ != static_cast<int>(latest_frame_.height);
        if (geometry_changed) release_layout_input(on_touch, on_keyboard, on_button);
        if (!update_layout(latest_crop_, display_degrees(latest_base_degrees_), true,
                           on_touch, on_keyboard, on_button)) return false;
        display_paused_ = false;
        if (!draw(latest_frame_, latest_crop_)) return false;
        // draw 因新像素几何设置的释放请求已经在本地动作中交付。
        if (geometry_changed) release_pending_ = false;
    }
    display_paused_ = !resume;
    paused_input_stale_ = false;
    if (resume) latest_frame_ = {};
    return true;
}

bool Presenter::readback(const std::string &path) {
    if (!video_playback_) return false;
    int out_w = 0, out_h = 0;
    if (!prepare_output() || SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0 ||
        out_w <= 0 || out_h <= 0) {
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
    // 回读始终使用完整绘制面，不启用自动转换鼠标事件的 logical size。
    if (!prepare_output()) {
        std::fprintf(stderr, SCRCTL_TR("Failed to configure viewport: %s\n"), SDL_GetError());
        SDL_UnlockSurface(s);
        SDL_FreeSurface(s);
        return false;
    }
    // 显式指定完整像素矩形；当前 SDL2 software 驱动在 rect=NULL 时曾发生段错误。
    const SDL_Rect full{0, 0, out_w, out_h};
    const int rc =
        SDL_RenderReadPixels(renderer_, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    const std::string read_error = rc != 0 ? SDL_GetError() : "";
    SDL_UnlockSurface(s);
    if (rc != 0) {
        std::fprintf(stderr, SCRCTL_TR("Readback failed: %s\n"), read_error.c_str());
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
                     "[input] %s raw(%d,%d) viewport %d x%d (rotation %d degrees, horizontal flip: %s) / window %d x%d / "
                     "drawable %d x%d -> (%.3f, %.3f)\n"),
                 tag, raw_x, raw_y, view_w_, view_h_, degrees_,
                 horizontal_flip_ ? SCRCTL_TR("yes") : SCRCTL_TR("no"), pw, ph, ow, oh, fx, fy);
}

bool Presenter::pump(const std::function<void(double, double, bool)> &on_touch,
                     const KeyboardHandler &on_keyboard, const PasteHandler &on_paste,
                     const ButtonHandler &on_button, const RotateHandler &on_rotate) {
    apply_pending_resize(on_touch, on_keyboard, on_button);
    if (release_pending_) {
        release_input(on_touch, on_keyboard, on_button);
        discard_queued_input();
    } else if (!src_.input_valid || paused_input_stale_) {
        // 触摸坐标未知不影响物理按键。仅触摸依赖有效面板坐标，键盘可继续保持。
        release_touch(on_touch);
    }
    SDL_Event e;
    bool quit = render_failed_;
    bool repaint_background = false;
    bool repaint_paused = false;
    // 合并同一轮的鼠标移动事件，仅发送最后一个位置，减少高采样率鼠标带来的
    // 重复 HID 报告。按下和抬起仍保留各自事件。
    bool pending_move = false;
    double px = 0, py = 0;
    const Uint32 window_id = window_ != nullptr ? SDL_GetWindowID(window_) : 0;
    const auto button_failed = [&] {
        // DOWN 可能只送达一部分；先尽力松开所有尝试过的 Consumer，再清其它输入。
        pending_move = false;
        release_input(on_touch, on_keyboard, on_button);
    };
    while (SDL_PollEvent(&e)) {
        // 退出决定之后仍排空事件队列，但不再处理键盘、焦点恢复或鼠标尾部。
        // 等待合并的移动也留给退出收尾丢弃，不在退出时补发新的按下。
        if (quit) continue;
        switch (e.type) {
        case SDL_QUIT:
            quit = true;
            break;
        case SDL_WINDOWEVENT:
            // 焦点、可见性变化只影响本窗口。另一窗口的事件（包括没有身份的
            // 合成事件）不能打断当前拖动，也不能丢弃本窗口等待合并的移动。
            if (window_id == 0 || e.window.windowID != window_id) break;
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                input_active_ = true;
                if (!(SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) minimized_ = false;
            } else if (e.window.event == SDL_WINDOWEVENT_RESTORED ||
                       e.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
                minimized_ = false;
            } else if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                quit = true;
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                       e.window.event == SDL_WINDOWEVENT_HIDDEN ||
                       e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                // 先清待发移动，再释放最后实际交付的触点；否则本轮循环末尾
                // 可能在抬起之后再次发送 down，或让抬起跳到未发送的位置。
                pending_move = false;
                input_active_ = false;
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) minimized_ = true;
                release_input(on_touch, on_keyboard, on_button);
            }
            if (apply_pending_resize(on_touch, on_keyboard, on_button)) pending_move = false;
            if (render_failed_) quit = true;
            if (!video_playback_ && (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SHOWN ||
                e.window.event == SDL_WINDOWEVENT_RESTORED ||
                e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) repaint_background = true;
            if (display_paused_ && (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SHOWN ||
                e.window.event == SDL_WINDOWEVENT_RESTORED ||
                e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) repaint_paused = true;
            break;
        case SDL_KEYDOWN: {
            if (!input_active_ || window_id == 0 || e.key.windowID != window_id) break;
            // 与 scrcpy 相同：快捷键使用选定的修饰键；普通字符及 Esc 留给设备输入。
            // 默认只选左 Alt / 左 Super，避免把 AltGr（右 Alt）误当作快捷键。
            const auto mods = e.key.keysym.mod;
            const bool shortcut = (mods & shortcut_mods_) != 0;
            // scrcpy 的 F11 在所有修饰组合下都归本地；只有无修饰的新 DOWN
            // 切换全屏，其余组合和对应 UP 也不能进入设备键盘状态。
            const bool local_f11 = e.key.keysym.sym == SDLK_F11;
            const bool fullscreen_key = local_f11 &&
                !(mods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT));
            const SDL_Scancode scancode = e.key.keysym.scancode;
            const bool fresh = !e.key.repeat && !keyboard_.is_pressed(scancode);
            const auto reports = keyboard_.key_down(scancode, mods, e.key.repeat != 0,
                                                     shortcut || local_f11);
            // 一次完整 DOWN/UP 也可能换控件或修改文本；按住门控只能保护中间状态。
            // 仅真实设备报告使旧粘贴失效，MOD+V 自身不能取消刚创建的请求。
            if (on_keyboard && !reports.empty()) ++input_generation_;
            for (const auto &report : reports) {
                if (on_keyboard) on_keyboard(report);
            }
            const int key_index = static_cast<int>(scancode);
            auto *button_key = key_index > 0 && key_index < SDL_NUM_SCANCODES
                ? &button_keys_[static_cast<size_t>(key_index)] : nullptr;
            // 只有已交付的音量键可重复 DOWN；后按 MOD 不能抢走原来的设备归属，
            // 本地几何清理已经发 UP 的键也不能由 repeat 再次按下。
            if (e.key.repeat && shortcut && !(mods & KMOD_SHIFT) && !button_failed_ &&
                on_button && button_key && button_key->down &&
                (button_key->usage == hid::button::kVolumeUp ||
                 button_key->usage == hid::button::kVolumeDown)) {
                ++input_generation_;
                if (!on_button(hid::button::kUsagePageConsumer, button_key->usage, true)) {
                    button_failed_ = true;
                    button_failed();
                }
            }
            // 所有权在首次 DOWN 确定。已转给设备的 F 不能因后来按下 MOD、
            // 或一个重复 DOWN 改成全屏动作；不支持的 scancode 也不触发本地动作。
            if (!fresh || !keyboard_.is_pressed(scancode)) break;
            if (shortcut && e.key.keysym.sym == SDLK_q) {
                quit = true;
            } else if (shortcut && e.key.keysym.sym == SDLK_v && !(mods & KMOD_SHIFT)) {
                if (on_paste) on_paste();
            } else if (shortcut && e.key.keysym.sym == SDLK_r && !(mods & KMOD_SHIFT)) {
                if (on_rotate) {
                    // 请求改变设备坐标依据前结束已有触点，避免跨方向保持拖动。
                    // 保留本地快捷键归属直到真实 UP；没有控制回调时只消费组合键。
                    release_layout_input(on_touch, on_keyboard, on_button);
                    pending_move = false;
                    on_rotate();
                }
            } else if (shortcut && e.key.keysym.sym == SDLK_z && video_playback_) {
                if (paused_input_stale_) pending_move = false;
                if (!refresh_display((mods & KMOD_SHIFT) != 0, on_touch, on_keyboard, on_button)) {
                    render_failed_ = true;
                    quit = true;
                }
            } else if ((shortcut && e.key.keysym.sym == SDLK_f && !(mods & KMOD_SHIFT)) ||
                       fullscreen_key) {
                release_layout_input(on_touch, on_keyboard, on_button);
                pending_move = false;
                toggle_fullscreen();
                repaint_background = !video_playback_;
                repaint_paused = display_paused_;
                if (apply_pending_resize(on_touch, on_keyboard, on_button)) pending_move = false;
                if (render_failed_) quit = true;
            } else if (shortcut && (mods & KMOD_SHIFT) && video_playback_ &&
                       (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT ||
                        e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN)) {
                const bool vertical = e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN;
                flip_display(vertical, on_touch, on_keyboard, on_button);
                pending_move = false;
                if (!draw_uploaded()) {
                    render_failed_ = true;
                    quit = true;
                }
            } else if (shortcut && !(mods & KMOD_SHIFT) &&
                       video_playback_ &&
                       (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT)) {
                const int step = e.key.keysym.sym == SDLK_RIGHT ? 90 : 270;
                const int degrees = (degrees_ + step) % 360;
                if (update_layout(src_, degrees, true, on_touch, on_keyboard, on_button)) {
                    rotation_offset_ = (rotation_offset_ + step) % 360;
                    pending_move = false;
                    if (!draw_uploaded()) {
                        render_failed_ = true;
                        quit = true;
                    }
                }
            } else if (shortcut && !(mods & KMOD_SHIFT) &&
                       (e.key.keysym.sym == SDLK_g || e.key.keysym.sym == SDLK_w)) {
                if (resize_window(e.key.keysym.sym == SDLK_g, on_touch, on_keyboard, on_button)) {
                    pending_move = false;
                }
            } else if (shortcut && !(mods & KMOD_SHIFT) && on_button &&
                       !button_failed_ && button_key) {
                uint16_t usage = 0;
                switch (e.key.keysym.sym) {
                case SDLK_h: usage = hid::button::kHome; break;
                case SDLK_p: usage = hid::button::kLock; break;
                case SDLK_UP: usage = hid::button::kVolumeUp; break;
                case SDLK_DOWN: usage = hid::button::kVolumeDown; break;
                default: break;
                }
                if (usage && !press_button(*button_key, usage, on_button)) button_failed();
            }
            break;
        }
        case SDL_KEYUP: {
            if (window_id == 0 || e.key.windowID != window_id) break;
            const int key_index = static_cast<int>(e.key.keysym.scancode);
            auto *button_key = key_index > 0 && key_index < SDL_NUM_SCANCODES
                ? &button_keys_[static_cast<size_t>(key_index)] : nullptr;
            const bool owned_button = button_key && button_key->usage != 0;
            bool button_ok = true;
            if (owned_button) {
                // 用原 DOWN 的映射释放；MOD 早松、Shift 改变不改变真实 UP 的目标。
                button_ok = release_button(*button_key, on_button);
                *button_key = {};
            }
            if (input_active_ || owned_button) {
                for (const auto &report : keyboard_.key_up(e.key.keysym.scancode,
                                                          e.key.keysym.mod)) {
                    if (on_keyboard) on_keyboard(report);
                }
            }
            if (!button_ok) button_failed();
            break;
        }
        case SDL_TEXTINPUT:
        case SDL_TEXTEDITING:
            // 物理模式已停用本进程 SDL 文本输入；此前排队的文字/IME 编辑
            // 也不能与物理键报告混合注入。不更改宿主机输入法或键盘布局。
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (input_active_ && window_id != 0 && e.button.windowID == window_id &&
                e.button.button == SDL_BUTTON_MIDDLE && on_button && !button_failed_ &&
                middle_home_.usage == 0) {
                if (!press_button(middle_home_, hid::button::kHome, on_button)) button_failed();
                break;
            }
            if (video_playback_ && input_active_ && window_id != 0 && e.button.windowID == window_id &&
                e.button.button == SDL_BUTTON_LEFT) {
                // 用原始窗口点换算绘制面，按显示共用的矩形判断留边。
                // 第一击也不注入触摸，双击只调整本地窗口；文件回放没有触摸
                // 回调时仍可使用此动作。触摸模拟鼠标不触发双击窗口动作。
                if (!is_content_point(e.button.x, e.button.y)) {
                    if (e.button.clicks == 2 && e.button.which != SDL_TOUCH_MOUSEID &&
                        resize_window(false, on_touch, on_keyboard, on_button)) {
                        pending_move = false;
                    }
                    break;
                }
                if (!on_touch) break;
                if (!to_display(e.button.x, e.button.y, px, py)) {
                    break;
                }
                ++input_generation_;
                dragging_ = true;
                last_touch_x_ = px;
                last_touch_y_ = py;
                if (debug_input_) {
                    report_input(e.button.x, e.button.y, px, py, SCRCTL_TR("down"));
                }
                on_touch(px, py, true);
            }
            break;
        case SDL_MOUSEMOTION:
            if (video_playback_ && input_active_ && window_id != 0 && e.motion.windowID == window_id &&
                dragging_ && on_touch) {
                if (!to_display(e.motion.x, e.motion.y, px, py)) {
                    pending_move = false;
                    release_touch(on_touch);
                    break;
                }
                if (debug_input_) {
                    report_input(e.motion.x, e.motion.y, px, py, SCRCTL_TR("move"));
                }
                pending_move = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (window_id != 0 && e.button.windowID == window_id &&
                e.button.button == SDL_BUTTON_MIDDLE && middle_home_.usage != 0) {
                const bool ok = release_button(middle_home_, on_button);
                middle_home_ = {};
                if (!ok) button_failed();
                break;
            }
            if (video_playback_ && input_active_ && window_id != 0 && e.button.windowID == window_id &&
                e.button.button == SDL_BUTTON_LEFT && dragging_ && on_touch) {
                if (pending_move) {
                    pending_move = false;
                    last_touch_x_ = px;
                    last_touch_y_ = py;
                    on_touch(px, py, true);
                }
                if (to_display(e.button.x, e.button.y, px, py)) {
                    last_touch_x_ = px;
                    last_touch_y_ = py;
                }
                release_touch(on_touch);
            }
            break;
        default:
            break;
        }
    }
    if (!quit && apply_pending_resize(on_touch, on_keyboard, on_button)) pending_move = false;
    if (render_failed_) quit = true;
    if (!quit && repaint_background && !draw_background()) {
        render_failed_ = true;
        quit = true;
    }
    if (!quit && repaint_paused && !draw_uploaded()) {
        render_failed_ = true;
        quit = true;
    }
    if (quit) {
        input_active_ = false;
        pending_move = false;
        release_input(on_touch, on_keyboard, on_button);
    } else if (pending_move && on_touch) {
        pending_move = false;
        last_touch_x_ = px;
        last_touch_y_ = py;
        on_touch(px, py, true);
    }
    return quit;
}

bool Presenter::is_fullscreen() const {
    return window_ != nullptr && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

int Presenter::display_degrees(int base_degrees) const {
    const int base = base_degrees % 360;
    return ((display_flip_offset_ ? -base : base) + rotation_offset_ + 360) % 360;
}

bool Presenter::is_windowed() const {
    return window_ != nullptr && !minimized_ &&
           !(SDL_GetWindowFlags(window_) &
             (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED));
}

void Presenter::discard_queued_input() {
    if (window_ == nullptr) return;
    Uint32 own_id = SDL_GetWindowID(window_);
    // 只过滤当前队列，不安装全局 filter；窗口状态、退出和其他窗口事件保持原样。
    SDL_FilterEvents([](void *userdata, SDL_Event *event) {
        const Uint32 id = *static_cast<Uint32 *>(userdata);
        Uint32 event_id = 0;
        switch (event->type) {
        case SDL_KEYDOWN: case SDL_KEYUP: event_id = event->key.windowID; break;
        case SDL_TEXTINPUT: event_id = event->text.windowID; break;
        case SDL_TEXTEDITING: event_id = event->edit.windowID; break;
        case SDL_MOUSEMOTION: event_id = event->motion.windowID; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: event_id = event->button.windowID; break;
        case SDL_MOUSEWHEEL: event_id = event->wheel.windowID; break;
        default: return 1;
        }
        return event_id == id ? 0 : 1;
    }, &own_id);
}

void Presenter::discard_queued_pointer() {
    if (!window_) return;
    struct PointerFilter { Uint32 id; bool keep_middle_up; };
    PointerFilter filter{SDL_GetWindowID(window_), middle_home_.usage != 0};
    SDL_FilterEvents([](void *userdata, SDL_Event *event) {
        const auto &filter = *static_cast<const PointerFilter *>(userdata);
        // 中键 HOME 没有旧坐标；局部布局已经交付 UP，但其实际抬起仍结束本地归属。
        if (filter.keep_middle_up && event->type == SDL_MOUSEBUTTONUP &&
            event->button.windowID == filter.id && event->button.button == SDL_BUTTON_MIDDLE)
            return 1;
        Uint32 event_id = 0;
        switch (event->type) {
        case SDL_MOUSEMOTION: event_id = event->motion.windowID; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: event_id = event->button.windowID; break;
        case SDL_MOUSEWHEEL: event_id = event->wheel.windowID; break;
        default: return 1;
        }
        return event_id == filter.id ? 0 : 1;
    }, &filter);
}

void Presenter::release_layout_input(const std::function<void(double, double, bool)> &on_touch,
                                     const KeyboardHandler &on_keyboard,
                                     const ButtonHandler &on_button) {
    ++input_generation_;
    const bool buttons_ok = release_buttons(on_button, true);
    release_touch(on_touch);
    // 失败的输入通路不能保留待结束的本地按钮归属；其它 UP 已在上面尽力发送。
    if (!buttons_ok) {
        button_keys_.fill({});
        middle_home_ = {};
    }
    for (const auto &report : buttons_ok ? keyboard_.release_device_keys()
                                       : keyboard_.release_all()) {
        if (on_keyboard) on_keyboard(report);
    }
    release_pending_ = false;
    discard_queued_pointer();
}

void Presenter::flip_display(bool vertical,
                             const std::function<void(double, double, bool)> &on_touch,
                             const KeyboardHandler &on_keyboard,
                             const ButtonHandler &on_button) {
    // 当前显示轴上的 H/V 作用在已有旋转之后：H*R(d)=R(-d)*H，V=R(180)*H。
    // 视口尺寸不变；0/180 度也必须清理旧坐标，不能被同布局 early return 吞掉。
    release_layout_input(on_touch, on_keyboard, on_button);
    const int step = vertical ? 180 : 0;
    degrees_ = (step - degrees_ + 360) % 360;
    rotation_offset_ = (step - rotation_offset_ + 360) % 360;
    display_flip_offset_ = !display_flip_offset_;
    horizontal_flip_ = !horizontal_flip_;
    if (resize_pending_) resize_preserve_local_ = true;
}

bool Presenter::resize_for_content(int old_w, int old_h, int new_w, int new_h) {
    int width = 0, height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    SDL_Rect usable{};
    const int display = SDL_GetWindowDisplayIndex(window_);
    if (display >= 0 && SDL_GetDisplayUsableBounds(display, &usable) != 0) usable = {};
    int wanted_w = width, wanted_h = height;
    if (!window_for_content(old_w, old_h, new_w, new_h, width, height, usable.w, usable.h,
                            wanted_w, wanted_h)) {
        std::fprintf(stderr, "%s\n", SCRCTL_TR(
            "Cannot resize window for new content; keeping previous dimensions"));
        return false;
    }
    if (width != wanted_w || height != wanted_h) {
        int x = 0, y = 0;
        SDL_GetWindowPosition(window_, &x, &y);
        SDL_SetWindowSize(window_, wanted_w, wanted_h);
        SDL_SetWindowPosition(window_, x, y);
    }
    SDL_GetWindowSize(window_, &win_w_, &win_h_);
    return true;
}

bool Presenter::update_content(const Crop &crop, int degrees,
                               const std::function<void(double, double, bool)> &on_touch,
                               const KeyboardHandler &on_keyboard,
                               const ButtonHandler &on_button) {
    if (!video_playback_ || !window_ || crop.w <= 0 || crop.h <= 0 ||
        (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270)) return false;
    const int base = (degrees - rotation_offset_ + 360) % 360;
    source_degrees_ = display_flip_offset_ ? (360 - base) % 360 : base;
    if (display_paused_) {
        if (!same_crop(crop, src_) || degrees != degrees_) {
            paused_input_stale_ = true;
            release_touch(on_touch);
            discard_queued_pointer();
        }
        return true;
    }
    return update_layout(crop, degrees, false, on_touch, on_keyboard, on_button);
}

bool Presenter::update_layout(const Crop &crop, int degrees, bool local,
                              const std::function<void(double, double, bool)> &on_touch,
                              const KeyboardHandler &on_keyboard,
                              const ButtonHandler &on_button) {
    if (!video_playback_) return false;
    int width = 0, height = 0;
    viewport_size(crop, degrees, width, height);
    if (window_ == nullptr || crop.w <= 0 || crop.h <= 0 ||
        (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270)) return false;
    // 来源坐标发生变化时，旧纹理不能作为新裁剪的已上传画面；等待新 Frame。
    if (!local && (crop.x != src_.x || crop.y != src_.y || crop.w != src_.w ||
                   crop.h != src_.h || crop.input_valid != src_.input_valid ||
                   crop.pixel_degrees != src_.pixel_degrees ||
                   crop.display_w != src_.display_w || crop.display_h != src_.display_h)) {
        texture_uploaded_ = false;
    }
    if (degrees == degrees_ && width == view_w_ && height == view_h_) return true;
    if (local) release_layout_input(on_touch, on_keyboard, on_button);
    else {
        release_input(on_touch, on_keyboard, on_button);
        discard_queued_input();
    }
    if (width != view_w_ || height != view_h_) {
        if (is_windowed()) {
            const int old_w = resize_pending_ ? windowed_content_w_ : view_w_;
            const int old_h = resize_pending_ ? windowed_content_h_ : view_h_;
            resize_for_content(old_w, old_h, width, height);
            resize_pending_ = false;
        } else if (!resize_pending_) {
            windowed_content_w_ = view_w_;
            windowed_content_h_ = view_h_;
            resize_pending_ = true;
        }
    }
    degrees_ = degrees;
    src_ = crop;
    view_w_ = width;
    view_h_ = height;
    if (resize_pending_) resize_preserve_local_ = local;
    return true;
}

bool Presenter::apply_pending_resize(const std::function<void(double, double, bool)> &on_touch,
                                     const KeyboardHandler &on_keyboard,
                                     const ButtonHandler &on_button) {
    if (!resize_pending_ || !is_windowed()) return false;
    const bool repaint = resize_preserve_local_ && texture_uploaded_;
    if (resize_preserve_local_ && !release_pending_) release_layout_input(on_touch, on_keyboard, on_button);
    else {
        release_input(on_touch, on_keyboard, on_button);
        discard_queued_input();
    }
    resize_for_content(windowed_content_w_, windowed_content_h_, view_w_, view_h_);
    resize_pending_ = false;
    resize_preserve_local_ = false;
    if (repaint && !draw_uploaded()) render_failed_ = true;
    return true;
}

void Presenter::toggle_fullscreen() {
    if (window_ != nullptr &&
        SDL_SetWindowFullscreen(window_, is_fullscreen() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        std::fprintf(stderr, SCRCTL_TR("Failed to change fullscreen mode: %s\n"), SDL_GetError());
    }
}

bool Presenter::is_content_point(int x, int y) const {
    int content_x = 0, content_y = 0;
    return to_content(x, y, content_x, content_y);
}

bool Presenter::prepare_output() const {
    // 软件 renderer 在窗口 resize 后可暂时返回旧 surface 尺寸。
    // 排入完整 viewport 并 flush，让 SDL 激活新绘制面，再重设完整 viewport。
    // 第一次命令可能带旧尺寸；放大窗口时不能让它裁掉新面远侧的像素。
    return renderer_ && SDL_RenderSetScale(renderer_, 1, 1) == 0 &&
           SDL_RenderSetViewport(renderer_, nullptr) == 0 && SDL_RenderFlush(renderer_) == 0 &&
           SDL_RenderSetViewport(renderer_, nullptr) == 0;
}

bool Presenter::output_content_rect(ContentRect &rect) const {
    if (!video_playback_ || !window_ || !renderer_ || minimized_ ||
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return false;
    int width = 0, height = 0;
    return prepare_output() && SDL_GetRendererOutputSize(renderer_, &width, &height) == 0 &&
           content_rect(view_w_, view_h_, width, height, rect);
}

bool Presenter::to_content(int raw_x, int raw_y, int &x, int &y) const {
    ContentRect content;
    if (!output_content_rect(content)) return false;
    float drawable_x = 0, drawable_y = 0;
    // logical size=0、scale=1、完整 viewport：此 API 只做窗口点到绘制面像素
    // 的 DPI 换算。不预先乘 DPI，也不读取当前鼠标位置猜队列中的旧事件。
    SDL_RenderWindowToLogical(renderer_, raw_x, raw_y, &drawable_x, &drawable_y);
    if (!std::isfinite(drawable_x) || !std::isfinite(drawable_y) ||
        drawable_x < content.x || drawable_y < content.y ||
        drawable_x >= int64_t(content.x) + content.w ||
        drawable_y >= int64_t(content.y) + content.h) return false;
    // 留边判断先于整数截断。实际内容第一列可以是 0；负半点不能混入这一列。
    x = static_cast<int>((double(drawable_x) - content.x) * view_w_ / content.w);
    y = static_cast<int>((double(drawable_y) - content.y) * view_h_ / content.h);
    return true;
}

bool Presenter::resize_window(bool pixel_perfect,
                              const std::function<void(double, double, bool)> &on_touch,
                              const KeyboardHandler &on_keyboard,
                              const ButtonHandler &on_button) {
    if (!video_playback_ || !window_ || !renderer_ || minimized_ ||
        (SDL_GetWindowFlags(window_) &
         (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) return false;
    if (!prepare_output()) return false;
    int points_w = 0, points_h = 0, drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSize(window_, &points_w, &points_h);
    if (SDL_GetRendererOutputSize(renderer_, &drawable_w, &drawable_h) != 0) return false;
    int width = 0, height = 0;
    const bool valid = pixel_perfect
        ? pixel_perfect_window(view_w_, view_h_, points_w, points_h,
                               drawable_w, drawable_h, width, height)
        : window_without_borders(view_w_, view_h_, points_w, points_h,
                                  drawable_w, drawable_h, width, height);
    if (!valid) return false;

    release_layout_input(on_touch, on_keyboard, on_button);
    if (width == points_w && height == points_h) return true;

    int x = 0, y = 0;
    if (!pixel_perfect) SDL_GetWindowPosition(window_, &x, &y);
    SDL_SetWindowSize(window_, width, height);
    SDL_GetWindowSize(window_, &win_w_, &win_h_);
    if (!pixel_perfect) {
        // 去留边后维持原内容中心；像素 1:1 动作只调整尺寸，不移动窗口。
        const auto centered = [](int position, int previous, int current) {
            return static_cast<int>(std::clamp(int64_t(position) + (int64_t(previous) - current) / 2,
                int64_t(std::numeric_limits<int>::min()), int64_t(std::numeric_limits<int>::max())));
        };
        SDL_SetWindowPosition(window_, centered(x, points_w, win_w_),
                               centered(y, points_h, win_h_));
    }
    return true;
}

void Presenter::release_touch(const std::function<void(double, double, bool)> &on_touch) {
    if (dragging_ && on_touch) {
        on_touch(last_touch_x_, last_touch_y_, false);
    }
    dragging_ = false;
}

void Presenter::release_input(const std::function<void(double, double, bool)> &on_touch,
                              const KeyboardHandler &on_keyboard,
                              const ButtonHandler &on_button) {
    ++input_generation_;
    release_buttons(on_button, false);
    release_touch(on_touch);
    for (const auto &report : keyboard_.release_all()) {
        if (on_keyboard) on_keyboard(report);
    }
    release_pending_ = false;
}

bool Presenter::ready_for_paste() const {
    return window_ != nullptr && input_active_ && !release_pending_ && !dragging_ &&
           keyboard_.held().empty() && middle_home_.usage == 0 &&
           std::none_of(button_keys_.begin(), button_keys_.end(),
                        [](const ButtonKey &key) { return key.usage != 0; });
}

bool Presenter::button_down(uint16_t usage) const {
    return (middle_home_.down && middle_home_.usage == usage) ||
        std::any_of(button_keys_.begin(), button_keys_.end(), [usage](const ButtonKey &key) {
            return key.down && key.usage == usage;
        });
}

bool Presenter::press_button(ButtonKey &key, uint16_t usage, const ButtonHandler &on_button) {
    const bool already_down = button_down(usage);
    // 先记录尝试，失败时同样尽力 UP。HOME 键和中键共持时只交付首 DOWN/末 UP。
    key = {usage, true};
    if (!already_down) {
        ++input_generation_;
        if (!on_button(hid::button::kUsagePageConsumer, usage, true)) {
            button_failed_ = true;
            return false;
        }
    }
    return true;
}

bool Presenter::release_button(ButtonKey &key, const ButtonHandler &on_button) {
    if (!key.down) return true;
    key.down = false;
    if (!button_down(key.usage) && on_button &&
        !on_button(hid::button::kUsagePageConsumer, key.usage, false)) {
        button_failed_ = true;
        return false;
    }
    return true;
}

bool Presenter::release_buttons(const ButtonHandler &on_button, bool preserve_local) {
    bool ok = true;
    for (auto &key : button_keys_) {
        if (!release_button(key, on_button)) ok = false;
        if (!preserve_local) key = {};
    }
    if (!release_button(middle_home_, on_button)) ok = false;
    if (!preserve_local) middle_home_ = {};
    return ok;
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
    if (physical_keyboard_mode_ && --physical_keyboard_windows == 0 && restore_text_input) {
        SDL_StartTextInput();
    }
}

bool Presenter::to_display(int raw_x, int raw_y, double &fx, double &fy) const {
    if (paused_input_stale_) return false;
    int content_x = 0, content_y = 0;
    if (!to_content(raw_x, raw_y, content_x, content_y)) return false;
    return scrctl::app::viewport_fraction_to_panel(content_x, content_y, src_, degrees_, fx, fy,
                                                  horizontal_flip_);
}

} // namespace scrctl::app
