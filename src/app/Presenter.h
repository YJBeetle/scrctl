#pragma once

#include "app/KeyboardState.h"
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
    using KeyboardHandler = std::function<void(const KeyboardState::Report &)>;
    using PasteHandler = std::function<void()>;
    /// Consumer 按钮的真实 DOWN/UP；false 表示发送失败，窗口会尽力释放其它输入。
    using ButtonHandler = std::function<bool(uint16_t, uint16_t, bool)>;

    /// 等比缩放时留边区域的背景色，在 open() 前设置；首帧前的 clear 也使用它。
    void set_background(uint8_t r, uint8_t g, uint8_t b);

    /// degrees 是窗口对源图像施加的顺时针旋转，用于视口和鼠标的首次逆变换。
    /// 截图已经应用的设备方向单独保留在 Crop.pixel_degrees 中，不能重复旋转。
    /// spec.horizontal_flip 在窗口旋转前作用于裁剪区域，不改变裁剪的源像素位置。
    bool open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
              bool scale_given, const WindowSpec &spec);
    /// 无视频播放的普通背景窗口。默认 256x256 点；显式宽高各自覆盖默认维度，
    /// 不推导视频比例或应用视频缩放/方向，不创建像素帧或视频纹理。
    /// 键盘、粘贴、全屏和退出沿用 pump；禁用坐标触摸及视频尺寸动作。
    bool open_background(const WindowSpec &spec);
    /// 原地更新内容方向和可见尺寸，保留 SDL 窗口身份、用户位置和特殊模式。
    /// 普通窗口按当前显示尺度适配；特殊模式等恢复普通窗口后再适配最终内容。
    /// 更新前释放旧输入并丢弃本窗口已排队的输入，使旧粘贴代次失效。
    bool update_content(const Crop &crop, int degrees,
                        const std::function<void(double, double, bool)> &on_touch,
                        const KeyboardHandler &on_keyboard = {},
                        const ButtonHandler &on_button = {});

    /// 返回 true 表示已消费有效帧；最小化或暂时没有绘制面时跳过显示。
    /// 指定回读路径时仍要求图像保存成功，不能把跳过当作已经保存。
    bool draw(const scrctl::Frame &f, const char *readback_path = nullptr);
    /// 画面来源或尺寸改变时更新裁剪、内容视口和纹理，保留窗口位置及拖动状态。
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

    /// 物理键盘报告保留全部按住的 usages；不将 SDL_TEXTINPUT 重复注入成文字。
    /// 成功打开窗口后停用本进程的 SDL 文本输入，最后一个 Presenter 销毁时
    /// 恢复原先启用状态；不更改宿主机输入法或键盘布局。
    bool pump(const std::function<void(double, double, bool)> &on_touch,
              const KeyboardHandler &on_keyboard = {}, const PasteHandler &on_paste = {},
              const ButtonHandler &on_button = {});
    /// 异步粘贴完成时必须再次检查焦点、按住状态和代次。释放输入及新设备
    /// DOWN 使旧代次失效，完整点击/按键抬起后也不能向已改变的上下文粘贴。
    [[nodiscard]] uint64_t input_generation() const { return input_generation_; }
    [[nodiscard]] bool ready_for_paste() const;
    /// 查询 SDL 的实际全屏状态，不缓存用户的窗口模式选择。
    [[nodiscard]] bool is_fullscreen() const;
    /// 将来源或显式方向与用户的本机旋转/镜像组合；保持到窗口关闭，不改录制方向。
    [[nodiscard]] int display_degrees(int base_degrees) const;
    /// pump 即时重绘失败时锁存，调用方应按渲染错误收尾，而非正常窗口退出。
    [[nodiscard]] bool render_failed() const { return render_failed_; }
    /// 在内容改变、退出或坐标依据失效时，释放最后一个有效的设备触摸点。
    void release_touch(const std::function<void(double, double, bool)> &on_touch);
    /// 在内容改变或退出前先释放 Consumer，再清触摸和键盘；重复调用不重复发送 UP。
    void release_input(const std::function<void(double, double, bool)> &on_touch,
                       const KeyboardHandler &on_keyboard = {},
                       const ButtonHandler &on_button = {});

    ~Presenter();

  private:
    bool open_window(const WindowSpec &spec);
    void begin_physical_keyboard_mode();
    bool draw_background();
    bool draw_uploaded(const char *readback_path = nullptr);
    bool refresh_display(bool resume,
                         const std::function<void(double, double, bool)> &on_touch,
                         const KeyboardHandler &on_keyboard, const ButtonHandler &on_button);
    /// 将 SDL 鼠标的窗口点坐标转换为设备整屏的 0..1 坐标。
    /// renderer 固定 logical size=0、scale=1 和完整绘制面视口。
    /// SDL_RenderWindowToLogical 先负责点到物理像素的 DPI 换算，再按与绘制
    /// 共用的内容矩形去掉留边、换算内容坐标。先用浮点判断留边，最后截整数，
    /// 避免不足一个逻辑单位的负坐标被 SDL 截成 0 后误发到设备边缘。
    /// 先逆窗口旋转和裁剪区域的水平翻转，加源像素裁剪偏移，
    /// 再逆截图自身的旋转，得到设备面板坐标。
    /// 截图的方向依据不完整时返回 false。
    bool to_display(int raw_x, int raw_y, double &fx, double &fy) const;
    bool to_content(int raw_x, int raw_y, int &x, int &y) const;
    bool output_content_rect(ContentRect &rect) const;
    bool prepare_output() const;
    bool ensure_texture(int width, int height);
    void toggle_fullscreen();
    bool is_windowed() const;
    bool resize_for_content(int old_w, int old_h, int new_w, int new_h);
    bool apply_pending_resize(const std::function<void(double, double, bool)> &on_touch,
                              const KeyboardHandler &on_keyboard,
                              const ButtonHandler &on_button);
    void discard_queued_input();
    void discard_queued_pointer();
    void release_layout_input(const std::function<void(double, double, bool)> &on_touch,
                              const KeyboardHandler &on_keyboard,
                              const ButtonHandler &on_button);
    void flip_display(bool vertical,
                      const std::function<void(double, double, bool)> &on_touch,
                      const KeyboardHandler &on_keyboard,
                      const ButtonHandler &on_button);
    bool update_layout(const Crop &crop, int degrees, bool local,
                       const std::function<void(double, double, bool)> &on_touch,
                       const KeyboardHandler &on_keyboard,
                       const ButtonHandler &on_button);
    bool is_content_point(int x, int y) const;
    /// 返回 true 表示已接受窗口尺寸动作；全屏、最大化、最小化时不执行。
    bool resize_window(bool pixel_perfect,
                       const std::function<void(double, double, bool)> &on_touch,
                       const KeyboardHandler &on_keyboard,
                       const ButtonHandler &on_button);

    struct ButtonKey {
        uint16_t usage = 0; // 非零表示本次 DOWN 已由本地消费，直到真实 UP 或完整清理。
        bool down = false; // 包含发送失败前已尝试的 DOWN，供尽力补发 UP。
    };
    bool button_down(uint16_t usage) const;
    bool press_button(ButtonKey &key, uint16_t usage, const ButtonHandler &on_button);
    bool release_button(ButtonKey &key, const ButtonHandler &on_button);
    bool release_buttons(const ButtonHandler &on_button, bool preserve_local);

    uint8_t bg_[3] = {0, 0, 0};
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *texture_ = nullptr;
    bool physical_keyboard_mode_ = false;
    bool video_playback_ = true;
    bool texture_uploaded_ = false;
    bool render_failed_ = false;
    // 冻结帧保留在 GPU；暂停期间只缓存一个最新来源帧，恢复后释放 CPU 缓冲。
    bool display_paused_ = false;
    bool paused_input_stale_ = false;
    Frame latest_frame_{};
    Crop latest_crop_{};
    int source_degrees_ = 0;
    int latest_base_degrees_ = 0;
    int texture_w_ = 0, texture_h_ = 0;
    Crop src_{};
    /// 窗口对源像素施加的顺时针角度，以及视口尺寸（90/270 时宽高对调）。
    int degrees_ = 0;
    int rotation_offset_ = 0;
    /// 本机镜像位于来源方向之后，因此镜像时后续来源角度也需要取反。
    bool display_flip_offset_ = false;
    bool horizontal_flip_ = false;
    int view_w_ = 0, view_h_ = 0;
    int win_w_ = 0, win_h_ = 0;
    /// 特殊模式中的第一次内容基准。中间多次变化不覆盖，恢复时只适配一次。
    bool resize_pending_ = false;
    /// 最近一次延后布局来自本地动作时，只清旧坐标，保留 Local 直到真实 UP。
    bool resize_preserve_local_ = false;
    int windowed_content_w_ = 0, windowed_content_h_ = 0;
    /// 无法获取显示器边界时保留原始窗口尺寸。
    static constexpr int win_w_fallback = 1 << 20;
    static constexpr int win_h_fallback = 1 << 20;
    /// 失焦、隐藏或最小化后暂停输入，直到本窗口重新获得焦点。
    bool input_active_ = true;
    /// 以本窗口的 MINIMIZED/RESTORED 事件补充 SDL flag，允许事件处理与绘制分开。
    bool minimized_ = false;
    bool dragging_ = false;
    /// 源坐标依据改变时，下一轮 pump 统一释放触摸和键盘一次。
    bool release_pending_ = false;
    double last_touch_x_ = 0, last_touch_y_ = 0;
    bool debug_input_ = false;
    uint16_t shortcut_mods_ = KMOD_LALT | KMOD_LGUI;
    KeyboardState keyboard_;
    std::array<ButtonKey, SDL_NUM_SCANCODES> button_keys_{};
    ButtonKey middle_home_{};
    bool button_failed_ = false;
    uint64_t input_generation_ = 0;
};

} // namespace scrctl::app
