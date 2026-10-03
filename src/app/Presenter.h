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
    /// 等比留边那两条边的颜色（`--background-color`）。要在 `open()` 之前设好：
    /// 首帧之前就有一次 clear，之后每次 draw 用它。
    void set_background(uint8_t r, uint8_t g, uint8_t b);

    /// `degrees` 是设备报的界面旋转（"要顺时针转多少才正立"）。它同时决定三件事：
    /// 窗口与 logical size 的**朝向**、渲染时的旋转、以及鼠标坐标的逆映射。
    /// 三者必须用同一个数，否则就是"画面转正了但点击还是歪的"。
    bool open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
              bool scale_given, const WindowSpec &spec);

    void draw(const scrctl::Frame &f, const char *readback_path = nullptr);

    /// 把真正呈现到窗口上的内容读回存盘。日志只能证明帧率，证明不了画面；
    /// 而裁剪/缩放/纹理尺寸这类错误恰恰只有回读才看得见。
    ///
    /// 尺寸必须问渲染器要**输出像素**，不能用窗口的逻辑点数：之前这里用的就是
    /// win_w_/win_h_，于是"内容只画满了左上四分之一"这种错自己完全看不出来——
    /// 读回来的恰好是自己画进去的那块，永远自洽。
    bool readback(const std::string &path);

    /// 泵一轮事件。触摸换算成"整块屏幕的 0..1 归一化坐标"再交出去——注入用的
    /// 就是这套坐标，与分辨率无关。
    ///
    /// 换算必须带上裁剪偏移：窗口看到的是显示区，而触摸面的 0..1 是相对**整块
    /// 屏幕**的。把窗口中间点成 0.5 只在"没裁剪"时才对，裁过之后要按裁剪框在
    /// 屏幕里的位置平移一遍，否则点哪儿都偏。
    void set_debug_input(bool on) { debug_input_ = on; }

    /// 原始坐标、实时窗口点数、实时绘制面像素、算出的归一化值，一行全打出来。
    /// 只有同时看到这四个数才能判断鼠标到底活在哪个坐标系里。
    void report_input(int raw_x, int raw_y, double fx, double fy, const char *tag) const;

    bool pump(const std::function<void(double, double, bool)> &on_touch);

    ~Presenter();

  private:
    /// 鼠标位置 -> 整块屏幕的 0..1。
    ///
    /// **原始值就是逻辑坐标**：设了 SDL_RenderSetLogicalSize 之后，SDL2 会把鼠标
    /// 事件换算到逻辑空间再交给我们（实测：窗口 457 点 / 绘制面 914 像素，而右下角
    /// 的原始坐标是 1121 x 2431 —— 正好是逻辑尺寸 1125x2436）。所以这里只剩
    /// "按旋转映回面板轴、加裁剪偏移、除以整块屏的尺寸"，那三件事全在
    /// `viewport_fraction_to_panel` 里，那边可以离线自检。
    ///
    /// 这里连续错过两次，都是擅自假设原始值活在点或像素空间再去除一遍，结果整体
    /// 差 2.46 倍。留一条运行期核对：万一某个 SDL 版本行为不同，越界会立刻显形。
    void to_display(int raw_x, int raw_y, double &fx, double &fy) const;

    uint8_t bg_[3] = {0, 0, 0};
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    Crop src_{};
    /// 顺时针转正角度，以及由它决定的视口尺寸（90/270 时宽高对调）。
    int degrees_ = 0;
    int view_w_ = 0, view_h_ = 0;
    int win_w_ = 0, win_h_ = 0;
    /// 拿不到显示器边界时的兜底：按原始尺寸处理，等于不缩。
    static constexpr int win_w_fallback = 1 << 20;
    static constexpr int win_h_fallback = 1 << 20;
    bool dragging_ = false;
    bool debug_input_ = false;
};

} // namespace scrctl::app
