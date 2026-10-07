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
};

class Presenter {
  public:
    /// 等比缩放时留边区域的背景色，在 open() 前设置；首帧前的 clear 也使用它。
    void set_background(uint8_t r, uint8_t g, uint8_t b);

    /// degrees 是将设备画面转正所需的顺时针角度，同步用于窗口方向、渲染旋转
    /// 和鼠标逆映射，三处必须一致。
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

    ~Presenter();

  private:
    /// 将 SDL 鼠标逻辑坐标转换为设备整屏的 0..1 坐标。
    /// SDL_RenderSetLogicalSize 会把鼠标事件映射到逻辑空间。例如窗口为 457 点、
    /// 绘制面 914 像素时，事件右下角仍接近逻辑尺寸 1125x2436。
    /// 此处只需逆旋转、加裁剪偏移并除以整屏尺寸，不能再次换算点数与像素。
    /// 运行期保留越界核对，便于发现不同 SDL 版本的行为变化。
    void to_display(int raw_x, int raw_y, double &fx, double &fy) const;
    bool ensure_texture(int width, int height);

    uint8_t bg_[3] = {0, 0, 0};
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    int texture_w_ = 0, texture_h_ = 0;
    Crop src_{};
    /// 顺时针转正角度，以及由它决定的视口尺寸（90/270 时宽高对调）。
    int degrees_ = 0;
    int view_w_ = 0, view_h_ = 0;
    int win_w_ = 0, win_h_ = 0;
    /// 无法获取显示器边界时保留原始窗口尺寸。
    static constexpr int win_w_fallback = 1 << 20;
    static constexpr int win_h_fallback = 1 << 20;
    bool dragging_ = false;
    bool debug_input_ = false;
};

} // namespace scrctl::app
