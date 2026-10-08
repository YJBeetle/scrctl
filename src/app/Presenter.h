#pragma once

#include "app/ViewGeom.h"
#include "decode/Decoder.h"
#include <SDL.h>
#include <functional>
#include <string>

namespace scrctl::app {

struct WindowSpec {
    std::string title = "scrctl";
    int want_w = 0;
    int want_h = 0;
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    bool always_on_top = false;
    bool borderless = false;
    bool fullscreen = false;
    bool want_readback = false;
    /// 先在源像素裁剪区域内水平翻转，再施加窗口旋转；不改变设备或录制数据。
    bool horizontal_flip = false;
    uint16_t shortcut_mods = KMOD_LALT | KMOD_LGUI;
};

class Presenter {
  public:
    /// 等比缩放时留边区域的背景色，在 open() 前设置；首帧前的 clear 也使用它。
    void set_background(uint8_t r, uint8_t g, uint8_t b);

    /// degrees 是窗口对源图像施加的顺时针旋转，用于视口和鼠标的首次逆变换。
    /// 截图已经应用的设备方向单独保留在 Crop.pixel_degrees 中，不能重复旋转。
    /// spec.horizontal_flip 在窗口旋转前作用于裁剪区域，不改变裁剪的源像素位置。
    bool open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
              bool scale_given, const WindowSpec &spec);

    /// 返回 true 表示已完成绘制；指定回读路径时，也要求图像保存成功。
    bool draw(const scrctl::Frame &f, const char *readback_path = nullptr);
    /// 画面来源或尺寸改变时更新裁剪、逻辑视口和纹理，保留窗口位置及拖动状态。
    bool draw(const scrctl::Frame &f, const Crop &crop, const char *readback_path = nullptr);

    /// 将窗口实际显示内容回读保存，用于检查裁剪、缩放和纹理尺寸。
    /// 使用渲染器的输出像素尺寸，不能用窗口逻辑点数，否则高 DPI 下只读取局部。
    bool readback(const std::string &path);

    /// 处理窗口事件，将触摸转换为相对设备整屏的 0..1 坐标。
    /// 转换包含裁剪偏移：窗口只显示裁剪区域，触摸坐标仍以整个设备屏幕为基准。
    void set_debug_input(bool on) { debug_input_ = on; }

    /// 调试输出包含事件原始坐标、窗口点数、绘制面像素和归一化结果，
    /// 用于核对 SDL 的坐标空间。
    void report_input(int raw_x, int raw_y, double fx, double fy, const char *tag) const;

    bool pump(const std::function<void(double, double, bool)> &on_touch);
    /// 查询 SDL 的实际状态，设备转屏重建窗口时保留用户选择的全屏模式。
    [[nodiscard]] bool is_fullscreen() const;
    /// 在重建窗口、退出或坐标依据失效时，释放最后一个有效的设备触摸点。
    void release_touch(const std::function<void(double, double, bool)> &on_touch);

    ~Presenter();

  private:
    /// 将 SDL 鼠标逻辑坐标转换为设备整屏的 0..1 坐标。
    /// SDL_RenderSetLogicalSize 会把鼠标事件映射到逻辑空间。例如窗口为 457 点、
    /// 绘制面 914 像素时，事件右下角仍接近逻辑尺寸 1125x2436。
    /// 先逆窗口旋转和裁剪区域的水平翻转，加源像素裁剪偏移，
    /// 再逆截图自身的旋转，得到设备面板坐标。
    /// 不能再次换算点数与像素；截图的方向依据不完整时返回 false。
    bool to_display(int raw_x, int raw_y, double &fx, double &fy) const;
    bool ensure_texture(int width, int height);
    void toggle_fullscreen();

    uint8_t bg_[3] = {0, 0, 0};
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    int texture_w_ = 0, texture_h_ = 0;
    Crop src_{};
    /// 窗口对源像素施加的顺时针角度，以及视口尺寸（90/270 时宽高对调）。
    int degrees_ = 0;
    bool horizontal_flip_ = false;
    int view_w_ = 0, view_h_ = 0;
    int win_w_ = 0, win_h_ = 0;
    /// 无法获取显示器边界时保留原始窗口尺寸。
    static constexpr int win_w_fallback = 1 << 20;
    static constexpr int win_h_fallback = 1 << 20;
    bool dragging_ = false;
    bool release_pending_ = false;
    double last_touch_x_ = 0, last_touch_y_ = 0;
    bool debug_input_ = false;
    uint16_t shortcut_mods_ = KMOD_LALT | KMOD_LGUI;
};

} // namespace scrctl::app
