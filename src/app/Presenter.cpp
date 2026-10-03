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
        // 留一条标题栏的余量，别让窗口刚好顶满屏幕。
        scrctl::app::fit_window(view_w_, view_h_, desk.w, desk.h - 60, scale, scale_given, win_w_,
                                win_h_);
        if (!scale_given && win_w_ < view_w_) {
            std::printf("屏幕只有 %dx%d 点，窗口缩到 %dx%d（--scale 可覆盖）\n", desk.w, desk.h,
                        win_w_, win_h_);
        }
    }

    Uint32 win_flags = SDL_WINDOW_ALLOW_HIGHDPI;
    // 全屏与无边框是**建的时候**给的 flag，不是建完再改：先建带边框的窗口再切
    // 桌面全屏，SDL 会把窗口尺寸留在旧的约束里，转屏重建时就成了"全屏但画面
    // 只占中间一块"。
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
        std::fprintf(stderr, "建窗口失败: %s\n", SDL_GetError());
        return false;
    }
    if (spec.always_on_top) {
        SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
    }
    // SDL2 的 Metal 后端不支持 SDL_RenderReadPixels——需要回读验证时
    // 直接建软件渲染器，否则 Present 后读回会无声 abort。
    const Uint32 flags = want_readback ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED;
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
    //
    // 这里要用**视口**尺寸（转 90/270 时宽高对调），不能用裁剪框尺寸：logical size
    // 一设，鼠标事件的坐标就落进这个空间，用它当分母的触摸换算才对得上画面。
    SDL_RenderSetLogicalSize(renderer_, view_w_, view_h_);
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
    // 纹理必须是**源帧尺寸**——整帧上传进按裁剪尺寸建的纹理会因尺寸不符
    // 而失败。裁剪与缩放统一交给 RenderCopy 的 src/dst 矩形表达。
    // BGRA 内存布局对应 little-endian 的 ARGB8888。
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                 frame_w, frame_h);
    if (texture_ == nullptr) {
        std::fprintf(stderr, "建纹理失败: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(texture_, SDL_ScaleModeBest);
    std::printf("窗口 %dx%d 点 / 绘制面 %dx%d 像素 / 视口 %dx%d（源帧 %dx%d，裁剪 %dx%d+%d+%d，"
                "转正顺时针 %d°）\n",
                win_w_, win_h_, out_w, out_h, view_w_, view_h_, frame_w, frame_h, crop.w, crop.h,
                crop.x, crop.y, degrees_);
    return true;
}

void Presenter::draw(const scrctl::Frame &f, const char *readback_path) {
    // `SDL_RenderClear` 清的是整块目标（不受 logical size 那块等比留边限制），
    // 所以背景色直接就把两条边涂上了。这一点是量出来的：本来以为要像回读那样
    // 先把 logical size 摘掉，去掉之后回读像素证明边上仍然是背景色。
    SDL_SetRenderDrawColor(renderer_, bg_[0], bg_[1], bg_[2], 255);
    SDL_RenderClear(renderer_);
    if (SDL_UpdateTexture(texture_, nullptr, f.pixels.data(), static_cast<int>(f.row_pitch)) != 0) {
        std::fprintf(stderr, "上传纹理失败: %s\n", SDL_GetError());
    }
    // 设了 logical size 之后渲染器坐标就是逻辑坐标，画满整个逻辑区域即可；
    // Retina 缩放和窗口拉伸后的等比留边由 SDL 负责。旋转在 draw_rotated 里做，
    // 那条路径与离线自检共用同一个函数。
    scrctl::app::draw_rotated(renderer_, texture_, src_, degrees_);
    // 必须在 Present 之前读：Present 之后后缓冲已交换，SDL_RenderReadPixels
    // 会读到失效内容并段错误。
    if (readback_path != nullptr) {
        readback(readback_path);
    }
    SDL_RenderPresent(renderer_);
}

bool Presenter::readback(const std::string &path) {
    int out_w = 0, out_h = 0;
    if (SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0 || out_w <= 0 || out_h <= 0) {
        std::fprintf(stderr, "问绘制面尺寸失败: %s\n", SDL_GetError());
        return false;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s == nullptr) {
        std::fprintf(stderr, "回读建面失败: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_LockSurface(s) != 0) {
        std::fprintf(stderr, "回读加锁失败: %s\n", SDL_GetError());
        SDL_FreeSurface(s);
        return false;
    }
    // 读之前必须把 logical size 摘掉。挂着它的时候 `SDL_RenderReadPixels` 的矩形
    // 是按**逻辑**坐标解释的：(0,0,out_w,out_h) 不再是整块输出，而是从内容区左上角
    // 起的另一块设备矩形——窗口比例与画面比例不一致时（有等比留边）读回来的就是
    // 一个偏移过的局部，边上那些像素根本不在读到的范围里（微测：内容区之外的
    // 1x1 读直接失败，内容区之内读到的是错位的东西）。
    //
    // 这个 bug 只在窗口比例与画面比例不同时才显形，而默认窗口是按画面比例算的，
    // 所以之前所有 `--verify` 的结论都恰好没被它影响。
    SDL_RenderSetLogicalSize(renderer_, 0, 0);
    // 显式给矩形：SDL2 的 software 驱动在 rect=NULL 时会段错误（实测）。
    const SDL_Rect full{0, 0, out_w, out_h};
    const int rc =
        SDL_RenderReadPixels(renderer_, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_RenderSetLogicalSize(renderer_, view_w_, view_h_);
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

void Presenter::report_input(int raw_x, int raw_y, double fx, double fy, const char *tag) const {
    int pw = 0, ph = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(window_, &pw, &ph);
    SDL_GetRendererOutputSize(renderer_, &ow, &oh);
    std::fprintf(stderr,
                 "[input] %s 原始(%d,%d) 视口%d x%d（转%d°）/ 窗口%d x%d / 绘制面%d x%d -> (%.3f, "
                 "%.3f)\n",
                 tag, raw_x, raw_y, view_w_, view_h_, degrees_, pw, ph, ow, oh, fx, fy);
}

bool Presenter::pump(const std::function<void(double, double, bool)> &on_touch) {
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
                if (debug_input_) {
                    report_input(e.button.x, e.button.y, px, py, "按下");
                }
                on_touch(px, py, true);
            }
            break;
        case SDL_MOUSEMOTION:
            if (dragging_ && on_touch) {
                to_display(e.motion.x, e.motion.y, px, py);
                if (debug_input_) {
                    report_input(e.motion.x, e.motion.y, px, py, "移动");
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
