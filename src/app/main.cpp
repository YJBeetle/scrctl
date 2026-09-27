// scrctl：把设备屏幕镜像到原生窗口。
//
// 两种源：真机实时流（默认，不带 --play 时）与已录制的 Annex-B 文件。两条路
// 共用同一个喂字节 -> 拆 AU -> 解码 -> 渲染的循环，区别只在"字节从哪来"。
//
// 解析器是同步的——一次性 feed 整个文件会在任何一帧被画出来之前就把上千帧
// 全解进内存（每帧 11MB）。所以分块喂，并用帧队列做背压。实时源同理：一次
// 只取一个数据报，喂完就回到事件循环，否则窗口会在等帧的时候冻住。
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/RenderPanel.h"
#include "app/ViewGeom.h"
#include "bitstream/AnnexB.h"
#include "decode/AudioDecoder.h"
#include "decode/Decoder.h"
#include "hid/Hid.h"
#include "media/AudioPump.h"
#include "media/FramePump.h"
#include "media/StreamSession.h"
#include "remote/App.h"
#include "remote/Device.h"
#include "remote/DisplayInfo.h"
#include "remote/Pasteboard.h"
#include "rt/RtpHevc.h"

namespace {

struct Options {
    std::string path;       ///< 空 = 走真机实时流
    std::string serial;     ///< scrcpy 的 --serial：指定哪台设备
    std::string record;     ///< 实时流顺手把 Annex-B 录到文件
    bool list_devices = false;
    bool no_control = false;  ///< scrcpy 的 --no-control：只看不动
    std::string title = "scrctl";
    bool stats = false;
    bool crop_set = false;
    int crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
    double scale = 1.0;   ///< 窗口相对裁剪尺寸的缩放
    bool scale_given = false;  ///< 显式给过 --scale 就别再自动缩进屏幕
    bool debug_input = false;  ///< 把每次鼠标事件的原始坐标与算出的归一化值都打出来
    int exit_after = 0;   ///< 渲染多少帧后退出（0=不限）
    int verify_at = 0;    ///< 渲染到第 N 帧时回读窗口内容
    std::string verify_path;
    /// 注入一条直线后退出：`--test-touch x0,y0,x1,y1`。
    /// 窗口与鼠标不在场时也要能验证输入通路，理由同 --verify：日志说"注入
    /// 调用返回成功"证明不了设备上真的收到了触摸。
    std::string test_touch;
    /// 起流后按一次硬件按键（home/lock/volup/voldn/mute），然后照常镜像。
    /// 按键效果是瞬时的，所以它要能和 --verify 组合：按完等第 N 帧回读窗口。
    std::string test_button;
    /// 起流后往设备敲一段 ASCII（要有文本框正获得焦点）。
    std::string test_type;
    /// scrcpy 的 --start-app=name：起流之后把某个 App 拉到前台。名字里可以带两个
    /// 前缀，语义照 scrcpy：`+` = 先杀掉在跑的实例再冷启动，`?` = 按 App 名字前缀
    /// 匹配（大小写不敏感）而不是按 bundle id 精确匹配。
    std::string start_app;
    bool list_apps = false;  ///< --list-apps：列出设备上装的 App 后退出
    std::string copy_text;   ///< --copy TEXT：写进设备剪贴板后退出
    bool paste = false;      ///< --paste：读设备剪贴板打印后退出
    bool no_window = false;  ///< --no-window：不起窗口，只收流（脚本/自动化用）
    /// --display-orientation：画面顺时针转这么多度。-1 = auto，跟着设备报的
    /// `currentOrientation` 走。
    int orientation = -1;
    int win_w = 0, win_h = 0;  ///< --window-width/height：显式窗口尺寸，0=自动
    /// 用平台硬件解码后端（VideoToolbox），而不是默认的软件解码。
    /// 见 FramePump::Options::use_hardware——默认软解的原因是硬解吃不下超过 65535 字节的帧。
    bool hw_decode = false;
    /// scrcpy 的 --no-audio：连音频腿都不起（不占设备上那条会话、不解码）。
    bool no_audio = false;
    /// --- 下面这批是窗口与运行控制的 scrcpy 同名项，逐个都是"照抄名字"级别的活 ---
    bool always_on_top = false;   ///< --always-on-top
    bool borderless = false;      ///< --window-borderless
    bool fullscreen = false;      ///< -f / --fullscreen（桌面全屏）
    int win_x = SDL_WINDOWPOS_CENTERED;  ///< --window-x
    int win_y = SDL_WINDOWPOS_CENTERED;  ///< --window-y
    /// --background-color=#RRGGBB：等比留边那两条边的颜色。默认黑。
    uint8_t bg[3] = {0, 0, 0};
    std::string render_driver;    ///< --render-driver（metal / software / ...）
    bool disable_screensaver = false;  ///< --disable-screensaver
    int time_limit = 0;           ///< --time-limit=秒，到点正常退出（会停流）
    bool show_version = false;    ///< --version
    /// scrcpy 的 --no-audio-playback：收流与解码照跑，只是不在电脑上出声。
    /// 录制或排障要"有音频数据但安静"时用它——本机夜里跑真机回归也靠它。
    bool no_audio_playback = false;
    /// scrcpy 的 --audio-buffer=ms（默认同为 50）。它同时是两件事的那一个数：
    /// 开口放之前先攒多久，以及缓冲想维持的水位（高出它就开始悄悄排）。
    int audio_buffer_ms = 50;
};

/// Ctrl-C 与 `kill` 应当让进程走正常退出路径（停流、关会话），而不是只能被 SIGKILL。
/// 这件事在 SDL 之后才成立：SDL 初始化时会接管 SIGINT/SIGTERM，而它接管之后没有任何
/// 东西转达给这个循环——实测 `kill -TERM`、`kill -INT` 和 Ctrl-C 都动不了它（进程照跑，
/// 最后只能 kill -9）。后果不只是"退不出去"：它一边跑一边占着设备那条媒体会话，而同一台
/// 设备同时只容得下一条（第二条 startmediastream 会把第一条顶掉，docs §13），于是两个
/// scrctl 互相拆对方的流——实测就是这样刷出"每 2.5 秒被设备结束一次流"的假象，而设备
/// 什么都没做错。
std::atomic<bool> g_stop_requested { false };

void on_stop_signal(int) { g_stop_requested = true; }

void usage(const char *argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "\n"
        "  (无参数)             镜像当前连接的设备\n"
        "  -s, --serial SERIAL  多台设备时指定哪一台（UDID）\n"
        "  --list-devices       列出在连设备后退出\n"
        "  --play FILE          改播已录制的 Annex-B HEVC 文件\n"
        "  -r, --record FILE    把实时流另存为 Annex-B\n"
        "  -n, --no-control     只显示不注入输入（默认下鼠标左键即触摸）\n"
        "  --no-audio           不起音频腿（默认起：设备系统输出转 AAC-ELD，本地解成\n"
        "                     48kHz 立体声）。设备只给这一种编码，所以 scrcpy 的\n"
        "                     --audio-codec / --audio-source / --audio-bit-rate 在这里\n"
        "                     没有对应项——编码档位是设备定的\n"
        "  --no-audio-playback  收流与解码照跑，只是不在电脑上出声（夜里跑真机回归用）。\n"
        "                     想在电脑上静音只能用这一条或本机输出音量——设备给的\n"
        "                     audioSystemOutput 是**音量之前**的抽头，手机上按音量键\n"
        "                     按到零，镜像里照样是满幅（docs §17.2 ②）\n"
        "  --audio-buffer=MS    攒够 MS 毫秒才开口放，默认 50（与 scrcpy 同一个默认值）。\n"
        "                     它也是缓冲想维持的水位：囤到两倍就悄悄排回去，所以这个数\n"
        "                     同时决定起始延迟与音画对齐的稳态。调小延迟更低但更容易欠载\n"
        "                     （断续）。上限 1000ms，超了会收到这一档并打一行说明——再高\n"
        "                     就不是缓冲而是把声音存起来晚点放，看口型已经对不上了\n"
        "  --no-window          不起窗口只收流（脚本/自动化用）\n"
        "  -f, --fullscreen     桌面全屏（无边框，跟随显示器）\n"
        "  --always-on-top      窗口置顶\n"
        "  --window-borderless  无边框窗口\n"
        "  --window-x N / --window-y N  窗口位置（默认居中）\n"
        "  --background-color=#RRGGBB   等比留边那两条边的颜色，默认黑\n"
        "  --render-driver=NAME 指定 SDL 的渲染驱动（metal / software / ...）\n"
        "  --disable-screensaver 运行期间不让本机息屏\n"
        "  --time-limit=SEC     到点正常退出（走停流与关会话那条路，不是 kill）\n"
        "  --version            打印版本后退出\n"
        "  --window-title TEXT  窗口标题（--title 同义）\n"
        "  --window-width N / --window-height N  显式窗口尺寸，默认按屏幕自动缩\n"
        "  --debug-input        打印每次鼠标的原始坐标与换算结果（定坐标问题时用）\n"
        "  --crop WxH+X+Y       裁剪区域（也吃 scrcpy 的 W:H:X:Y；默认自动裁 CTU 填充）\n"
        "  --display-orientation=auto|0|90|180|270\n"
        "                       画面顺时针转多少度（--orientation 同义）。默认 auto：\n"
        "                       跟着设备报的界面旋转走，转屏时窗口自己换向。度数是顺时针，\n"
        "                       与 scrcpy 同义；两处偏差——scrcpy 还认 flip*（先水平翻转\n"
        "                       再转）我们没做，而 scrcpy 的 --orientation 会连带设录制\n"
        "                       方向，我们录的就是设备发来的原始码流，没有这一项\n"
        "  --scale F            窗口缩放系数，默认 1.0\n"
        "  --hw-decode          改用平台硬件解码（VideoToolbox）。默认是软件解码：\n"
        "                     硬解只吃 2 字节 NAL 长度前缀，而这条流单帧能到\n"
        "                     256KB，装不下的帧会被丢掉并重起会话\n"
        "  --title TITLE        窗口标题\n"
        "  --stats              每秒打印帧率统计\n"
        "  --exit-after N       渲染 N 帧后退出\n"
        "  --verify N FILE      渲染到第 N 帧时把窗口内容回读存为 BMP\n"
        "  --test-touch X0,Y0,X1,Y1\n"
        "                     注入一条直线（归一化坐标）后退出，无需真鼠标\n"
        "  --test-button NAME 起流后按一次硬件按键（home/lock/volup/voldn/mute），\n"
        "                     再照常镜像，配 --verify 才能看见瞬时效果\n"
        "  --test-type TEXT   起流后往设备敲一段 ASCII（需要已聚焦的文本框）\n"
        "  --list-apps        列出设备上安装的 App（bundle id 与名字）后退出\n"
        "  --start-app=NAME   起流后把某个 App 拉到前台。NAME 是 bundle id；\n"
        "                     前缀 ? 改成按 App 名字前缀匹配（大小写不敏感），\n"
        "                     前缀 + 表示先杀掉在跑的实例（两个可叠用，顺序 ? 在前）\n"
        "  --copy TEXT        把文本写进设备剪贴板后退出（中文走这条路）\n"
        "  --paste            读设备剪贴板并打印后退出；与 --copy 同用时写完读回\n"
        "\n"
        "这些 scrcpy 的选项在这条路上没有对应项，三组原因各不相同：\n"
        "  --max-size / --resolution / --max-fps / --video-bit-rate / --video-codec\n"
        "                     编码尺寸、码率、帧率是**设备定的**，不是能谈的：offer 里的\n"
        "                     pair_index 从 0 扫到 6，answer 一直回 1136x2464；把码率表中\n"
        "                     那档 6000000 改成 60000000，answer 依旧回 TXMaxBitrate 6000000。\n"
        "                     删档更糟——会退到表里 f2=299 那条，实测掉到 0.1Mbps/8fps。\n"
        "                     判据与数字见 tools/bitrate_probe 与 docs §11，别照猜测改。\n"
        "  --mouse / --mouse-bind / --keyboard\n"
        "                     注入只有一条路（HID），没有“用哪种设备仿真”这一层；而鼠标\n"
        "                     当前只有左键绑到触摸，右键/中键/滚轮没有绑定，所以也没有\n"
        "                     一份可改的键位表。\n"
        "  --turn-screen-off / --power-off-on-close / --screen-off-timeout / --tcpip /\n"
        "  --port / --camera-* / --v4l2-* / --new-display / --otg\n"
        "                     Android 侧的机制（电源管理、adb 转发、虚拟相机与虚拟屏），\n"
        "                     CoreDevice 这条路里没有对应的服务。\n",
        argv0);
}

bool parse_args(int argc, char **argv, Options &o) {
    // scrcpy 的选项两种写法都收：`--scale 0.5` 与 `--scale=0.5`。在这里一次性拆成
    // 前者，而不是每个选项各自处理一遍等号——漏一个的表现为"未知参数"，而用户抄的
    // 正是 scrcpy 的用法。只按第一个等号切，值里再带等号（--title=a=b）不受影响。
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const auto eq = a.find('=');
        if (a.rfind("--", 0) == 0 && eq != std::string::npos && eq + 1 < a.size()) {
            args.push_back(a.substr(0, eq));
            args.push_back(a.substr(eq + 1));
        } else {
            args.push_back(std::move(a));
        }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "%s 缺少参数值\n", what);
                std::exit(2);
            }
            return args[++i].c_str();
        };
        if (a == "--play") {
            o.path = next("--play");
        } else if (a == "-s" || a == "--serial") {
            o.serial = next("--serial");
        } else if (a == "-r" || a == "--record") {
            o.record = next("--record");
        } else if (a == "--list-devices") {
            o.list_devices = true;
        } else if (a == "-n" || a == "--no-control") {
            o.no_control = true;
        } else if (a == "--list-apps") {
            o.list_apps = true;
        } else if (a == "--start-app") {
            o.start_app = next("--start-app");
        } else if (a == "--version") {
            o.show_version = true;
        } else if (a == "-f" || a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--always-on-top") {
            o.always_on_top = true;
        } else if (a == "--window-borderless") {
            o.borderless = true;
        } else if (a == "--window-x" && i + 1 < args.size()) {
            o.win_x = std::atoi(next("--window-x"));
        } else if (a == "--window-y" && i + 1 < args.size()) {
            o.win_y = std::atoi(next("--window-y"));
        } else if (a == "--background-color") {
            unsigned r = 0, g = 0, b = 0;
            const std::string v = next("--background-color");
            if (std::sscanf(v.c_str(), "#%2x%2x%2x", &r, &g, &b) != 3) {
                std::fprintf(stderr, "--background-color 要的是 #RRGGBB，收到 %s\n", v.c_str());
                return false;
            }
            o.bg[0] = static_cast<uint8_t>(r);
            o.bg[1] = static_cast<uint8_t>(g);
            o.bg[2] = static_cast<uint8_t>(b);
        } else if (a == "--render-driver") {
            o.render_driver = next("--render-driver");
        } else if (a == "--disable-screensaver") {
            o.disable_screensaver = true;
        } else if (a == "--time-limit") {
            o.time_limit = std::atoi(next("--time-limit"));
        } else if (a == "--no-audio") {
            o.no_audio = true;
        } else if (a == "--no-audio-playback") {
            o.no_audio_playback = true;
        } else if (a == "--audio-buffer") {
            o.audio_buffer_ms = std::atoi(next("--audio-buffer"));
        } else if (a == "--no-window") {
            o.no_window = true;
        } else if (a == "--window-title" || a == "--title") {
            o.title = next("--window-title");
        } else if (a == "--window-width" && i + 1 < args.size()) {
            o.win_w = std::atoi(next("--window-width"));
        } else if (a == "--window-height" && i + 1 < args.size()) {
            o.win_h = std::atoi(next("--window-height"));
        } else if (a == "--hw-decode") {
            o.hw_decode = true;
        } else if (a == "--debug-input") {
            o.debug_input = true;
        } else if (a == "--scale") {
            o.scale = std::atof(next("--scale"));
            o.scale_given = true;
        } else if (a == "--stats") {
            o.stats = true;
        } else if (a == "--exit-after") {
            o.exit_after = std::atoi(next("--exit-after"));
        } else if (a == "--verify") {
            o.verify_at = std::atoi(next("--verify"));
            o.verify_path = next("--verify");
        } else if (a == "--test-touch") {
            o.test_touch = next("--test-touch");
        } else if (a == "--test-button") {
            o.test_button = next("--test-button");
        } else if (a == "--test-type") {
            o.test_type = next("--test-type");
        } else if (a == "--copy") {
            o.copy_text = next("--copy");
        } else if (a == "--paste") {
            o.paste = true;
        } else if (a == "--display-orientation" || a == "--orientation") {
            // scrcpy 4.1 的 --orientation 是 --display-orientation + --record-orientation
            // 的合写。我们没有"录制方向"这件事（录的就是设备发来的原始码流），
            // 所以两个拼写都收、都只改显示——但 help 里要写清这一条偏差。
            const std::string v = next(a.c_str());
            if (v == "auto") {
                o.orientation = -1;
            } else if (v == "0" || v == "90" || v == "180" || v == "270") {
                o.orientation = std::atoi(v.c_str());
            } else {
                std::fprintf(stderr,
                             "--%s 只认 auto/0/90/180/270，收到 %s（scrcpy 还支持 flip*，"
                             "我们没做水平翻转）\n",
                             a.c_str() + 2, v.c_str());
                return false;
            }
        } else if (a == "--crop") {
            const char *v = next("--crop");
            if (std::sscanf(v, "%dx%d+%d+%d", &o.crop_w, &o.crop_h, &o.crop_x, &o.crop_y) != 4 &&
                std::sscanf(v, "%d:%d:%d:%d", &o.crop_w, &o.crop_h, &o.crop_x, &o.crop_y) != 4) {
                std::fprintf(stderr, "--crop 格式应为 WxH+X+Y 或 scrcpy 的 W:H:X:Y，收到 %s\n", v);
                return false;
            }
            o.crop_set = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "未知参数 %s\n", a.c_str());
            return false;
        }
    }
    if (o.scale <= 0.0) {
        o.scale = 1.0;
    }
    return true;
}

/// 设备编码分辨率比逻辑显示大（HEVC CTU 对齐填充），所以画面要按逻辑尺寸裁；
/// 裁剪框与坐标换算的几何在 ViewGeom.h，那里可以离线自检。
using scrctl::app::Crop;

/// 建窗口的那批参数。为什么要打包成一个结构而不是继续往 `open()` 后面加形参：
/// 它已经带 9 个参数，其中三个是相邻的 bool——再加位置与全屏，调用点上就没有人
/// 能靠读那一行判断第 7 个参数是什么了（而转屏重建那条路必须和首开那条**完全**
/// 用同一组窗口设置，否则转一次屏，置顶/无边框就悄悄丢了）。
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
    void set_background(uint8_t r, uint8_t g, uint8_t b) {
        bg_[0] = r;
        bg_[1] = g;
        bg_[2] = b;
    }

    /// `degrees` 是设备报的界面旋转（"要顺时针转多少才正立"）。它同时决定三件事：
    /// 窗口与 logical size 的**朝向**、渲染时的旋转、以及鼠标坐标的逆映射。
    /// 三者必须用同一个数，否则就是"画面转正了但点击还是歪的"。
    bool open(int frame_w, int frame_h, const Crop &crop, int degrees, double scale,
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
                std::printf("屏幕只有 %dx%d 点，窗口缩到 %dx%d（--scale 可覆盖）\n", desk.w,
                            desk.h, win_w_, win_h_);
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
        const Uint32 flags =
            want_readback ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED;
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
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
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

    void draw(const scrctl::Frame &f, const char *readback_path = nullptr) {
        // `SDL_RenderClear` 清的是整块目标（不受 logical size 那块等比留边限制），
        // 所以背景色直接就把两条边涂上了。这一点是量出来的：本来以为要像回读那样
        // 先把 logical size 摘掉，去掉之后回读像素证明边上仍然是背景色。
        SDL_SetRenderDrawColor(renderer_, bg_[0], bg_[1], bg_[2], 255);
        SDL_RenderClear(renderer_);
        if (SDL_UpdateTexture(texture_, nullptr, f.pixels.data(), static_cast<int>(f.row_pitch)) !=
            0) {
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

    /// 把真正呈现到窗口上的内容读回存盘。日志只能证明帧率，证明不了画面；
    /// 而裁剪/缩放/纹理尺寸这类错误恰恰只有回读才看得见。
    ///
    /// 尺寸必须问渲染器要**输出像素**，不能用窗口的逻辑点数：之前这里用的就是
    /// win_w_/win_h_，于是"内容只画满了左上四分之一"这种错自己完全看不出来——
    /// 读回来的恰好是自己画进去的那块，永远自洽。
    bool readback(const std::string &path) {
        int out_w = 0, out_h = 0;
        if (SDL_GetRendererOutputSize(renderer_, &out_w, &out_h) != 0 || out_w <= 0 ||
            out_h <= 0) {
            std::fprintf(stderr, "问绘制面尺寸失败: %s\n", SDL_GetError());
            return false;
        }
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32,
                                                        SDL_PIXELFORMAT_ARGB8888);
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
        const int rc = SDL_RenderReadPixels(renderer_, &full, SDL_PIXELFORMAT_ARGB8888, s->pixels,
                                            s->pitch);
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

    /// 泵一轮事件。触摸换算成"整块屏幕的 0..1 归一化坐标"再交出去——注入用的
    /// 就是这套坐标，与分辨率无关。
    ///
    /// 换算必须带上裁剪偏移：窗口看到的是显示区，而触摸面的 0..1 是相对**整块
    /// 屏幕**的。把窗口中间点成 0.5 只在"没裁剪"时才对，裁过之后要按裁剪框在
    /// 屏幕里的位置平移一遍，否则点哪儿都偏。
    void set_debug_input(bool on) { debug_input_ = on; }

    /// 原始坐标、实时窗口点数、实时绘制面像素、算出的归一化值，一行全打出来。
    /// 只有同时看到这四个数才能判断鼠标到底活在哪个坐标系里。
    void report_input(int raw_x, int raw_y, double fx, double fy, const char *tag) const {
        int pw = 0, ph = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(window_, &pw, &ph);
        SDL_GetRendererOutputSize(renderer_, &ow, &oh);
        std::fprintf(stderr,
                     "[input] %s 原始(%d,%d) 视口%d x%d（转%d°）/ 窗口%d x%d / 绘制面%d x%d -> (%.3f, "
                     "%.3f)\n",
                     tag, raw_x, raw_y, view_w_, view_h_, degrees_, pw, ph, ow, oh, fx, fy);
    }

    bool pump(const std::function<void(double, double, bool)> &on_touch) {
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

    ~Presenter() {
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
    void to_display(int raw_x, int raw_y, double &fx, double &fy) const {
        scrctl::app::viewport_fraction_to_panel(raw_x, raw_y, src_, degrees_, fx, fy);
    }

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

// ---------------------------------------------------------------- 帧的来源 ----

/// 一次 next 取到一帧已解码的图像。
///
/// 抽象成"取一帧"而不是"取一段字节"：字节层的事（分块喂、AU 边界、解码）现在
/// 两边各自解决了——实时走库里的 FramePump，文件走自己的解析器加解码器。主循环
/// 只关心"有没有新画面可以画"。
class FrameSource {
public:
    virtual ~FrameSource() = default;

    /// 返回 false 表示这次没取到（超时），调用方应该去泵一遍事件循环。
    /// `finished()` 为真才是真的结束。实现可以阻塞一小会儿再返回 false——
    /// 否则消费循环会在两帧之间空转，吃满一颗核还把窗口饿出事件事件。
    virtual bool next(scrctl::Frame &out, int timeout_ms) = 0;

    [[nodiscard]] virtual bool finished() const { return false; }
    /// 文件回放要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
    [[nodiscard]] virtual bool paces_itself() const { return false; }
    /// 打一段读数。实现方自己按调用间隔算速率，所以调用方只管按秒催。
    virtual void print_stats() {}

    /// 这块画面在设备上真正占多大（**可见区**，不是编码帧）。0/0 = 问不到。
    ///
    /// 只有实时源问得到：它是起流之前向设备的 `displayinfoupdates` 要来的。文件回放
    /// 没有设备可问，退回默认实现给 0，调用方再退回兜底表。
    virtual void display_size(int &width, int &height) const {
        width = 0;
        height = 0;
    }

    /// 画面要**顺时针**转多少度才正立。0 = 竖屏，或问不到。
    ///
    /// 为什么必须由源来报而不是由窗口自己看：编码帧**永远不转**（这台设备上横竖屏
    /// 都是 1136x2464），转屏只体现在设备报的 `currentOrientation` 上。所以窗口里
    /// 没有任何线索可推——不问就是横屏 App 躺倒。
    ///
    /// 注意它**不影响可见区尺寸**：实测界面转到 rot270 时 `currentMode.size` 仍是
    /// 1125x2436，只有朝向字段变了。所以裁剪框不用跟着换向（docs §16）。
    virtual int orientation_degrees() const { return 0; }
};

/// 首帧到手后定下"看哪一块"。
///
/// 可见区尺寸的**来源顺序**是这条路径的重点：
/// 1. 设备自己报的（`FrameSource::display_size`，起流前向 displayinfoupdates 要的）；
/// 2. 问不到才退回 `media::display_crop` 那张按机型硬编码的表。
/// 表里只有我们量过的那一档（1136x2464 -> 1125x2436），别的机型落到表外就是整幅当
/// 可见区——右边/下边留一条垃圾边，而触摸分母也跟着错。所以第 1 条能走就一定走它。
Crop resolve_crop(const Options &o, const scrctl::Frame &f, const FrameSource &source) {
    int display_w = 0, display_h = 0;
    source.display_size(display_w, display_h);
    const bool from_device = display_w > 0 && display_h > 0;
    if (!from_device) {
        const auto fallback = scrctl::media::display_crop(static_cast<int>(f.width),
                                                          static_cast<int>(f.height));
        display_w = fallback.w;
        display_h = fallback.h;
    }
    if (!from_device && !o.crop_set &&
        (static_cast<int>(f.width) != display_w || static_cast<int>(f.height) != display_h)) {
        std::printf("可见区 %ux%u -> %dx%d（按机型硬编码的兜底表：问设备没问到）\n", f.width,
                    f.height, display_w, display_h);
    }
    // 几何与夹取全在 ViewGeom.h 的 make_crop 里，那边可以离线自检。
    return scrctl::app::make_crop(o.crop_set, o.crop_x, o.crop_y, o.crop_w, o.crop_h,
                                  static_cast<int>(f.width), static_cast<int>(f.height), display_w,
                                  display_h);
}


/// 已录制的 Annex-B 文件。
///
/// 解析器是同步的——一次性 feed 整个文件会在任何一帧画出来之前就把上千帧全解进
/// 内存（每帧 11MB）。所以分块喂，并且解码结果攒在一个有上限的队列里。
class FileSource final : public FrameSource {
public:
    explicit FileSource(std::string path) : path_(std::move(path)) {}

    bool next(scrctl::Frame &out, int timeout_ms) override {
        (void)timeout_ms;  // 文件不会"等不到"，只会有"读完了"
        std::string err;
        while (frames_.empty() && !done_ && !pump_bytes(err)) {
            if (!err.empty()) {
                std::fprintf(stderr, "%s\n", err.c_str());
                done_ = true;
                return false;
            }
        }
        if (frames_.empty()) {
            return false;
        }
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    [[nodiscard]] bool finished() const override { return done_ && frames_.empty(); }
    [[nodiscard]] bool paces_itself() const override { return true; }

private:
    /// 读一块、喂给解析器、让回调往队列里放帧。队列满了就停手，下次再喂。
    bool pump_bytes(std::string &err) {
        if (!opened_ && !open_file(err)) {
            return false;
        }
        if (pos_ >= buffer_.size()) {
            // 收尾必须 flush：AU 的边界靠"下一个图像的起始 slice"判定，最后一个
            // AU 没有下一个，不 flush 就永远等不到它。
            if (!flushed_) {
                flushed_ = true;
                parser_->flush();
                done_ = true;
            }
            return false;
        }
        if (frames_.size() >= kMaxQueued) {
            return true;  // 背压：解码结果攒够了，先让调用方把它们画掉
        }
        const std::size_t n = std::min(kChunk, buffer_.size() - pos_);
        parser_->feed(buffer_.data() + pos_, n);
        pos_ += n;
        return true;
    }

    bool open_file(std::string &err) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            err = "打不开 " + path_;
            return false;
        }
        buffer_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        opened_ = true;
        std::printf("读入 %s (%zu 字节)\n", path_.c_str(), buffer_.size());

        decoder_ = scrctl::create_platform_decoder();
        if (decoder_ == nullptr) {
            // 没有后端时这里必须断掉而不是往下走：`on_au` 里第一件事就是
            // `decoder_->configure(...)`，而文件回放这条路上没人替它兜底
            // （实时流那条在 FramePump 里查了同一件事）。
            err = scrctl::kNoDecoderMessage;
            return false;
        }
        std::printf("解码后端: %s\n", decoder_->backend_name());
        parser_ = std::make_unique<scrctl::AnnexBParser>(
            [this](std::vector<scrctl::Nal> &&au, bool) { this->on_au(std::move(au)); });
        return true;
    }

    void on_au(std::vector<scrctl::Nal> &&au) {
        if (!configured_) {
            scrctl::Nal vps, sps, pps;
            for (const auto &n : au) {
                if (n.size() < 2) {
                    continue;
                }
                switch ((n[0] >> 1) & 0x3F) {
                    case 32: vps = n; break;
                    case 33: sps = n; break;
                    case 34: pps = n; break;
                    default: break;
                }
            }
            if (vps.empty() || sps.empty() || pps.empty() ||
                !decoder_->configure(vps, sps, pps)) {
                return;
            }
            configured_ = true;
        }
        scrctl::Frame f;
        if (decoder_->decode(au, f) && f) {
            frames_.push_back(std::move(f));
        }
    }

    static constexpr std::size_t kChunk = 48 * 1024;
    /// 队列上限。一帧 11MB，攒太多只是把内存吃掉而画面并不会更连贯。
    static constexpr std::size_t kMaxQueued = 8;

    std::string path_;
    std::vector<uint8_t> buffer_;
    std::deque<scrctl::Frame> frames_;
    std::unique_ptr<scrctl::Decoder> decoder_;
    std::unique_ptr<scrctl::AnnexBParser> parser_;
    std::size_t pos_ = 0;
    bool opened_ = false;
    bool configured_ = false;
    bool flushed_ = false;
    bool done_ = false;
};

/// 音频的 SDL 出口：按系统要的节拍从 AudioPump 里取 PCM。
///
/// 为什么这一层在 app 而不在 media：拿到 PCM 之后怎么放（声卡 / 写文件 / 丢掉的）是
/// 客户端的事，而"起流、解码、攒缓冲、回 RR"对任何客户端都一样——控制单元那条路就
/// 只要泵不要声卡。
class AudioOut {
public:
    AudioOut() = default;
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;
    ~AudioOut() { close(); }

    /// 打开默认输出设备。水位由泵自己定（`AudioPump::preroll_frames()`），这里不再
    /// 从外面传毫秒数——否则"攒多久"这件事会有两处换算，而它们会分家。
    ///
    /// 协商是**逐项对死**的（见下面的判据）：这一层没有重采样器，所以只有"系统给的
    /// 就是我们送的"这一种情况能开口放。
    ///
    /// 为什么要 preroll：一开口就取，第一个回调必然赶上"缓冲里才两三个包"的时刻，
    /// 于是起始十几毫秒全是补静音的接缝，听感是一声咔。攒 50ms 再放就把它压成
    /// 起始延迟——这也是 scrcpy 那个默认值存在的原因。
    bool open(scrctl::media::AudioPump &pump, std::string &err) {
        pump_ = &pump;
        channels_ = pump.channels() > 0 ? pump.channels() : 2;
        preroll_ = pump.preroll_frames();
        SDL_AudioSpec want {};
        want.freq = static_cast<int>(pump.sample_rate());
        want.format = AUDIO_S16SYS;
        want.channels = static_cast<Uint8>(channels_);
        // 一次回调 1024 帧 ≈ 48kHz 下 21ms。更小会被系统追不上（xrun），更大只是
        // 把延迟搬到设备侧的缓冲里。SDL 允许就近挑，实际值在 have 里。
        want.samples = 1024;
        want.callback = &AudioOut::fill;
        want.userdata = this;
        SDL_AudioSpec have {};
        // 不传 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE：我们要送的是 48kHz 的样本，而这里没有
        // 重采样器。让系统把设备开成 44.1kHz 只意味着同样的样本以 0.92 倍速放出去，
        // 现场表现是**音调低半档**——一个没人会往"出口协商"上想的症状。宁可开不了设备
        // 并说人话（调用方会退回"只收不放"）。声道数与采样格式同理不能迁就。
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dev_ == 0) {
            pump_ = nullptr;
            err = SDL_GetError();
            return false;
        }
        if (have.freq != want.freq || have.channels != want.channels ||
            have.format != want.format) {
            SDL_CloseAudioDevice(dev_);
            dev_ = 0;
            pump_ = nullptr;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "声卡只肯给 %d Hz / %d 声道 / 格式 0x%x，而这条流是 %d Hz / "
                          "%d 声道 / 0x%x——这里没有重采样器",
                          have.freq, have.channels, static_cast<unsigned>(have.format),
                          want.freq, want.channels, static_cast<unsigned>(want.format));
            err = buf;
            return false;
        }
        SDL_PauseAudioDevice(dev_, 0);
        std::printf("音频出口已开：%d Hz / %d 声道，后端 %s\n", have.freq, have.channels,
                    SDL_GetCurrentAudioDriver());
        return true;
    }

    /// 关设备。**必须在 SDL_Quit 之前**调到——之后音频子系统已经拆了，
    /// 再 SDL_CloseAudioDevice 就是对着已释放的上下文操作。
    void close() {
        if (dev_ != 0) {
            SDL_CloseAudioDevice(dev_);
            dev_ = 0;
        }
        pump_ = nullptr;
    }

    [[nodiscard]] bool dev_open() const { return dev_ != 0; }
    [[nodiscard]] uint64_t delivered() const { return delivered_.load(); }
    [[nodiscard]] uint64_t silence() const { return silence_.load(); }

private:
    static void fill(void *userdata, Uint8 *stream, int len) {
        auto *self = static_cast<AudioOut *>(userdata);
        const std::size_t frame_bytes = sizeof(int16_t) * static_cast<std::size_t>(self->channels_);
        const std::size_t frames = frame_bytes == 0 ? 0 : static_cast<std::size_t>(len) / frame_bytes;
        auto *dst = reinterpret_cast<int16_t *>(stream);
        if (self->pump_ == nullptr || frames == 0) {
            std::memset(dst, 0, static_cast<std::size_t>(len));
            return;
        }
        // 只在这个"还没开口"的判据上用缓冲水位；一旦开口就不再重新攒——中途发现
        // 欠载就补静音继续，比停下来重新攒 50ms 更接近"实时"，代价是最坏情况下
        // 一段接缝的咔声，而那正好是 silence 这一位要报出来的东西。
        if (!self->started_.load(std::memory_order_relaxed) &&
            self->pump_->buffered_frames() < self->preroll_) {
            std::memset(dst, 0, frames * frame_bytes);
            self->silence_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }
        self->started_.store(true, std::memory_order_relaxed);
        const std::size_t got = self->pump_->read(dst, frames);
        if (got < frames) {
            std::memset(dst + got * self->channels_, 0, (frames - got) * frame_bytes);
            self->silence_.fetch_add(frames - got, std::memory_order_relaxed);
        }
        self->delivered_.fetch_add(got, std::memory_order_relaxed);
    }

    scrctl::media::AudioPump *pump_ = nullptr;
    SDL_AudioDeviceID dev_ = 0;
    int channels_ = 2;
    std::size_t preroll_ = 0;
    std::atomic<bool> started_ { false };
    std::atomic<uint64_t> delivered_ { 0 };
    std::atomic<uint64_t> silence_ { 0 };
};

/// 真机实时流。收包、拆 AU、解码、以及"画面坏了就重起会话"全在 FramePump 里，
/// 这里只管起流、取帧、以及把窗口的输入投回设备。
class LiveSource final : public FrameSource {
public:
    ~LiveSource() override;

    /// `watch_display`：挂一条常驻订阅跟着转屏改朝向。不起窗口就别挂——那条
    /// 订阅要独占一个连接、还有一个每 250ms 醒一次的线程，而没有窗口就没人
    /// 消费朝向，纯开销。
    ///
    /// `want_audio`：起不起音频腿。它是**另一条设备侧会话**，起不来或者这个构建
    /// 根本没有音频后端都不致命——没有声音的镜像仍然是可用的镜像，所以这里只打一行。
    /// `audio_buffer_ms` = `--audio-buffer`：缓冲想维持的水位。
    bool start(const std::string &serial, const std::string &record_path, bool hw_decode,
               bool watch_display, bool want_audio, int audio_buffer_ms, std::string &err);

    /// 打开声卡。要和 `start()` 分开的唯一原因：`start()` 跑在 `SDL_Init` 之前
    /// （窗口还没建就得先有源），而 SDL 的音频子系统在那之后才有。
    bool start_playback(std::string &err);

    void stop_playback() { audio_out_.close(); }

    /// 起流前向设备要来的可见区尺寸（问不到是 0/0，见 `resolve_crop` 的顺序）。
    void display_size(int &width, int &height) const override {
        width = display_w_;
        height = display_h_;
    }

    /// 当前该顺时针转多少度。
    ///
    /// 优先问常驻订阅（`watcher_`），拿不到才退回起流前问到的那一档。顺序不能反：
    /// 常驻订阅是唯一会跟着转屏动的来源，而那一档是窗口打开那一刻的快照。
    ///
    /// 只有朝向是"活的"。可见区尺寸仍然只在起流前问一次——实测转屏时设备报的
    /// `currentMode.size` 根本不变（docs §16.1），而中途改尺寸要重建裁剪框、
    /// 触摸分母与整条几何日志，那些路径现在一条都没验过。
    [[nodiscard]] int orientation_degrees() const override {
        if (watcher_ != nullptr) {
            const auto st = watcher_->latest();
            if (!st.orientation.empty()) {
                return scrctl::app::orientation_degrees(st.orientation);
            }
        }
        return degrees_;
    }

    bool next(scrctl::Frame &out, int timeout_ms) override {
        if (pump_ == nullptr) {
            return false;
        }
        const uint64_t got = pump_->newer(out, serial_, timeout_ms);
        if (got == 0) {
            return false;
        }
        serial_ = got;
        return true;
    }

    [[nodiscard]] bool has_audio() const { return audio_ != nullptr; }

    /// 给"起流之后还要对设备做点别的"那些项用（--start-app）。它故意返回引用而不是
    /// 让每个功能自己存一份：一个会话只有一个 Device，多副本只会多一处要同步的寿命。
    [[nodiscard]] scrctl::remote::Device &device() { return *device_; }

    /// 把窗口里的一次触摸投到设备上。
    ///
    /// HID 服务**第一次用到时才连**：连接要一个来回，没必要把它算进起流路径；
    /// 而且设备不提供该服务（DDI 版本差异）时镜像应当照常工作，而不是整个退出。
    /// 失败过一次就不再重试，免得每帧都去撞一遍。
    bool control(double x, double y, bool down, std::string &err);

    /// 往设备敲一段 ASCII（复用触摸那条连接，键盘是同一个服务下的另一个面）。
    bool type_text(const std::string &text, int hold_ms, std::string &err);

    /// 按一个硬件按键（indigo 服务，惰性连）。按键的效果多半是瞬时的，所以
    /// 它得能和 `--verify` 组合使用：先按键，再等第 N 帧回读窗口内容。
    bool button(uint16_t usage_page, uint16_t usage_code, std::string &err);

    /// **必须打速率，不能打累计数。** 第一版这里打的是累计包数，结果"包 1778"
    /// 被当成每秒读数读了 16 秒，直接把结论带偏到"设备只编 12 帧"上——而它真正的
    /// 意思是这一段里我们一共只收到 110 包/秒。一个没有分母的数不是读数。
    void print_stats() override {
        if (pump_ == nullptr) {
            return;
        }
        const auto st = pump_->stats();
        const uint64_t now = SDL_GetTicks64();
        const double secs = last_stats_ms_ == 0
                                ? 1.0
                                : std::max(0.001, static_cast<double>(now - last_stats_ms_) / 1000.0);
        const auto rate = [&](uint64_t now_value, uint64_t before) {
            return static_cast<double>(now_value - before) / secs;
        };
        // 设备的 SR 每 `RTCPSendInterval` 秒才来一个（实测空闲时会拖到 4 秒以上），
        // 所以它的增量**不能**除以打印窗口，否则一次增量被摊成一秒的速率，数会虚高
        // 好几倍。除以"上一次 SR 变化到现在"的真实间隔，并且把这个间隔一起打出来。
        double dev_rate = 0;
        uint64_t dev_span_ms = 0;
        // 重起会话会让设备侧的累计数归零，做差会下溢成一个天文数字。
        const bool dev_reset = st.dev_sent_packets < last_dev_packets_;
        if (dev_reset) {
            last_dev_packets_ = st.dev_sent_packets;
            last_dev_change_ms_ = now;
            last_dev_rate_ = 0;
        } else if (st.dev_sent_packets != last_dev_packets_ && last_dev_change_ms_ != 0) {
            dev_span_ms = now - last_dev_change_ms_;
            dev_rate = static_cast<double>(st.dev_sent_packets - last_dev_packets_) /
                       std::max(0.001, dev_span_ms / 1000.0);
        } else {
            // 这一档没有新的 SR，沿用上一个 SR 算出来的速率——分母也就还是它的分母。
            dev_rate = last_dev_rate_;
            dev_span_ms = last_dev_span_ms_;
        }
        // 本会话收到的包数。设备 SR 里的累计数是**每条会话从零重数**的，而我们的
        // packets 全程连着涨，所以只有减掉基线两者才在同一条数轴上——以前直接打
        // 全程累计，重起过一次之后读数长成"累计 设备 143 我 5866"，像丢了五千包。
        const uint64_t mine_session =
            st.packets > st.session_packets_base ? st.packets - st.session_packets_base : 0;
        std::printf("  流: 设备发了 %6.0f/s 我收到 %6.0f/s | AU %5.1f/s 解码 %5.1f/s\n", dev_rate,
                    rate(st.packets, last_packets_), rate(st.aus, last_aus_),
                    rate(st.decoded, last_decoded_));
        // 两个"每秒"的分母不是一把尺：SR 大约每秒才来一个，它的增量只能除以"上一个
        // SR 到现在"，而我们的速率除以打印窗口（实测这个窗口在 0.6~1.3 秒之间飘）。
        // 所以这两个数**相减没有意义**——早先那行 `差 +283 / -283` 就是把它们硬减出来
        // 的，一虚一实读成"在大量丢包"，而真正的丢包读数在下面那行 `序号缺口` 上，
        // 全程是 0。这里把两个分母都打出来，谁看谁会别再犯。
        if (dev_span_ms == 0) {
            std::printf("      分母：设备那档还没有 SR 可除（第一条 SR 未到），我 %.1fs\n", secs);
        } else {
            std::printf("      分母：设备 %.1fs（SR 每 ~1s 一个） 我 %.1fs（两档相减无意义）\n",
                        dev_span_ms / 1000.0, secs);
        }
        // 这一行的两个数是唯一在同一条数轴上的读数（都按会话起点归零），所以它是
        // "设备到底发了多少 vs 我们收到多少"的权威比。AU/解码不在这个轴上：它们
        // 全程连着涨，没有会话基线，所以老实标成"全程"。
        std::printf("      本会话累计 设备 %llu 我 %llu｜全程 AU %llu 解码 %llu\n",
                    static_cast<unsigned long long>(st.dev_sent_packets),
                    static_cast<unsigned long long>(mine_session),
                    static_cast<unsigned long long>(st.aus),
                    static_cast<unsigned long long>(st.decoded));
        std::printf("      每帧耗时：拆包 %.1f ms 解码 %.1f ms 交付 %.1f ms（AU %llu 次）\n",
                    (st.ms_depacketize) / std::max<uint64_t>(1, st.packets),
                    st.ms_decode / std::max<uint64_t>(1, st.decode_calls),
                    st.ms_publish / std::max<uint64_t>(1, st.decode_calls),
                    static_cast<unsigned long long>(st.decode_calls));
        // 计数器不是一套基线，混在一行里就会读出"重起之后非视频载荷从 19 变成 0，
        // 是不是把 SR 弄丢了"这种假问题：前三个跟着拆包器每会话归零（拆包器换会话就
        // 重建），后四个全程累加。分开标。
        std::printf("      本会话 非视频载荷 %llu 序号缺口 %llu 分片作废 %llu\n",
                    static_cast<unsigned long long>(st.other_payload),
                    static_cast<unsigned long long>(st.gaps),
                    static_cast<unsigned long long>(st.dropped_fragments));
        std::printf("      全程 未出帧 %llu 等关键帧丢 %llu 重起 %llu 超大NAL丢 %llu\n",
                    static_cast<unsigned long long>(st.no_output),
                    static_cast<unsigned long long>(st.dropped_awaiting_keyframe),
                    static_cast<unsigned long long>(st.restarts),
                    static_cast<unsigned long long>(st.dropped_oversized));
        // 泵自己按数据报开头分的两类，跨会话连着涨。这两个数是"画面在不在变"的读数：
        // 视频那一档停下来不动而 SR 照每秒一个，就是屏幕静止（流还活着）；两档都停，
        // 才是设备把流结束掉了。
        // 后面那一档是**我们往外发**的续命 RR：设备的会话计时器只在收到它的时候复位，
        // 所以"流为什么断了"先看这三个数的哪一档停了。
        std::printf("      全程 视频数据报 %llu SR 心跳 %llu 发出 RR %llu PLI %llu（视频档停=画面静止，"
                    "SR 也停=流死了，RR 不涨=我们没在续命）\n",
                    static_cast<unsigned long long>(st.video_packets),
                    static_cast<unsigned long long>(st.sr_packets),
                    static_cast<unsigned long long>(st.rtcp_sent),
                    static_cast<unsigned long long>(st.pli_sent));
        if (audio_ != nullptr) {
            const auto as = audio_->stats();
            // 和上面同一把尺：速率除以这次打印窗口，累计数标"全程"。音频腿的分母
            // 天生比视频稳——设备在没有声音的时候**照发**包（实测 100 包/秒、20 秒
            // 一秒不多），所以这一行的"包"是平的，一旦它掉到 0 就是流死了。
            std::printf("  音频: 包 %6.0f/s 解出 %6.0f/s 交付 %6.0f 帧/s（出口=%s）\n",
                        rate(as.packets, last_audio_packets_),
                        rate(as.decoded, last_audio_decoded_),
                        rate(audio_out_.delivered(), last_audio_delivered_),
                        audio_out_.dev_open() ? SDL_GetCurrentAudioDriver() : "未开");
            std::printf("      全程 解败 %llu 真丢 %llu 迟到 %llu 丢旧 %llu 调速 %llu 补静音 %llu "
                        "RR %llu/%llu 重起 %llu 缓冲 %zu 帧\n",
                        static_cast<unsigned long long>(as.decode_failed),
                        static_cast<unsigned long long>(as.seq_lost),
                        static_cast<unsigned long long>(as.out_of_order),
                        static_cast<unsigned long long>(as.dropped_stale),
                        static_cast<unsigned long long>(as.steered),
                        static_cast<unsigned long long>(audio_out_.silence()),
                        static_cast<unsigned long long>(as.rtcp_sent),
                        static_cast<unsigned long long>(as.rtcp_failed),
                        static_cast<unsigned long long>(as.restarts),
                        audio_->buffered_frames());
            last_audio_packets_ = as.packets;
            last_audio_decoded_ = as.decoded;
            last_audio_delivered_ = audio_out_.delivered();
        }
        last_packets_ = st.packets;
        if (st.dev_sent_packets != last_dev_packets_) {
            last_dev_rate_ = dev_rate;
            last_dev_span_ms_ = dev_span_ms;
            last_dev_change_ms_ = now;
            last_dev_packets_ = st.dev_sent_packets;
        }
        last_aus_ = st.aus;
        last_decoded_ = st.decoded;
        last_stats_ms_ = now;
    }

private:
    std::unique_ptr<scrctl::remote::Device> device_;
    std::unique_ptr<scrctl::media::FramePump> pump_;
    /// 音频腿与声卡出口。它们都引用 `Device&` / `AudioPump`，所以**必须声明在
    /// device_ 之后**（成员反序析构：泵要先停、线程要先 join，才能拆 Device）。
    std::unique_ptr<scrctl::media::AudioPump> audio_;
    AudioOut audio_out_;
    uint64_t last_packets_ = 0;
    uint64_t last_dev_packets_ = 0;
    uint64_t last_dev_change_ms_ = 0;
    double last_dev_rate_ = 0;
    /// 上一个 SR 增量是除以多长的间隔算出来的。打印时要用它，不然读者会把这个
    /// "每秒"当成和"我收到"同一个分母，然后去减两个不同分母的数。
    uint64_t last_dev_span_ms_ = 0;
    uint64_t last_aus_ = 0;
    uint64_t last_decoded_ = 0;
    uint64_t last_audio_packets_ = 0;
    uint64_t last_audio_decoded_ = 0;
    uint64_t last_audio_delivered_ = 0;
    uint64_t last_stats_ms_ = 0;
    /// 起流之前向设备要来的**可见区**尺寸（0/0 = 没问到）。见 `display_size()`。
    int display_w_ = 0;
    int display_h_ = 0;
    /// 同一问带回来的界面旋转（顺时针度数）。0 也是有效值（竖屏），所以它不像尺寸
    /// 那样用"零"表示没问到——没问到就是 0，正立竖屏也是 0，两者行为本来就该一样。
    int degrees_ = 0;
    /// 尺寸是从哪块屏拿的，只为把日志那行说全（多屏设备上这不是废话：主屏与
    /// 无线屏的尺寸实测就不一样）。
    uint64_t display_id_ = 0;
    std::string display_name_;

    /// 常驻的显示几何订阅。它持有 `Device&`，所以**必须声明在 device_ 之后**
    /// （成员按声明反序析构，它得比 Device 先走）。起不来不致命：朝向就退回
    /// 起流前那一档，代价是"转屏不跟着转"。
    std::unique_ptr<scrctl::remote::DisplayWatcher> watcher_;

    std::unique_ptr<scrctl::hid::Service> hid_;
    std::unique_ptr<scrctl::hid::Buttons> buttons_;
    bool hid_unavailable_ = false;
    uint64_t serial_ = 0;
};

LiveSource::~LiveSource() = default;

bool LiveSource::start(const std::string &serial, const std::string &record_path, bool hw_decode,
                       bool watch_display, bool want_audio, int audio_buffer_ms,
                       std::string &err) {
    auto dev = scrctl::remote::Device::establish(serial, err);
    if (!dev) {
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*dev));

    scrctl::media::FramePump::Options options;
    options.record_path = record_path;
    options.use_hardware = hw_decode;

    // 显示几何先问设备，再起流。
    //
    // 为什么要问：编码帧的尺寸是"可见区 + HEVC 的 CU 对齐填充"，而这一圈填充多大
    // 协议里没有。此前我们按机型硬编码一档（1136x2464 -> 1125x2436），表外的机型
    // 就把整幅编码帧当可见区——后果是右/下一条垃圾边，而**触摸分母跟着错**，
    // 边缘点不准。`displayinfoupdates` 给的是设备的权威值。
    //
    // 为什么排在起流之前：这一问只要一条 deviceinfo 连接，与媒体会话无关，却要一个
    // 来回；放到起流之后就是让窗口多黑屏一个来回的时间。
    //
    // 问不到不致命：`resolve_crop` 会退回那张表，并把"是兜底"一起打出来。
    {
        std::string derr;
        const auto info = scrctl::remote::fetch_display_info(*device_, derr);
        const scrctl::remote::Display *d =
            info == std::nullopt ? nullptr : info->find(options.display_id);
        if (info != std::nullopt && d == nullptr) {
            // id 对不上时退回主屏：外接屏的 displayId 是设备分配的，不保证连续。
            d = info->primary();
        }
        if (d != nullptr && d->width > 0 && d->height > 0) {
            display_w_ = d->width;
            display_h_ = d->height;
            display_id_ = d->id;
            display_name_ = d->name;
            degrees_ = scrctl::app::orientation_degrees(d->orientation);
        } else {
            std::fprintf(stderr, "向设备问显示几何失败: %s（退回按机型硬编码的裁剪表）\n",
                         derr.empty() ? "推送里没有可用的尺寸" : derr.c_str());
        }
    }

    // 起流前那一问只够定下"窗口打开时该转多少度"。设备之后转屏我们一无所知，
    // 所以还要有人一直挂在订阅上——它是推模型，不挂着就再也没有第二条消息。
    //
    // 失败只打一行不返回 false：没有它，画面仍然按起流前那一档转正，只是不会
    // 跟着转屏走。为一个增强功能把镜像整个停掉是不划算的。
    if (watch_display) {
        std::string werr;
        watcher_ = scrctl::remote::DisplayWatcher::start(*device_, display_id_, werr, false);
        if (watcher_ == nullptr) {
            std::fprintf(stderr, "常驻显示订阅起不来: %s（转屏不会跟着转）\n", werr.c_str());
        }
    }

    pump_ = scrctl::media::FramePump::start(*device_, options, err);
    if (pump_ == nullptr) {
        return false;
    }
    scrctl::Frame first;
    if (!pump_->latest(first, 5000)) {
        err = "5 秒内没解出第一帧";
        return false;
    }
    std::printf("流已建立：%s / iOS %s，收流端口=%u PT=%u，首帧 %ux%u\n",
                device_->property("ProductType").c_str(), device_->property("OSVersion").c_str(),
                pump_->receiver_port(), pump_->payload_type(), first.width, first.height);
    // 打在这里而不是打在 `resolve_crop` 里，是因为控制单元那条路根本没有窗口：
    // "几何到底是设备报的还是那张兜底表"必须是**任何**跑法都能一眼看到的读数。
    if (display_w_ > 0) {
        std::printf("显示几何：设备报可见区 %dx%d（displayId=%llu %s），界面旋转顺时针 %d°，码流 %ux%u\n",
                    display_w_, display_h_, static_cast<unsigned long long>(display_id_),
                    display_name_.c_str(), degrees_, first.width, first.height);
    }
    if (!record_path.empty()) {
        std::printf("录制到 %s\n", record_path.c_str());
    }

    // 音频腿排在视频腿之后：它要一个 RPC 来回（实测 80~100ms），而窗口的第一帧不该
    // 为声音等这一下。苹果是反过来先起音频的，但那条路为什么不断已经查到别处了
    // （是我们的 UDP 拼装错了，docs §13），顺序在这件事上没有作用。
    //
    // 起不来只打一行、不改返回值：`--no-audio` 之外的失败（设备拒了、非 Apple 平台
    // 没有后端）都不该让整个镜像退出。
    if (want_audio) {
        if (!scrctl::kHaveAudioDecoder) {
            std::fprintf(stderr, "%s\n", scrctl::kNoAudioDecoderMessage);
        } else {
            scrctl::media::AudioPump::Options ao;
            ao.target_backlog_ms = audio_buffer_ms;
            std::string aerr;
            audio_ = scrctl::media::AudioPump::start(*device_, ao, aerr);
            if (audio_ == nullptr) {
                std::fprintf(stderr, "音频腿起不来: %s（画面照常，只是没有声音）\n",
                             aerr.c_str());
            } else {
                std::printf("音频腿已建立：收流端口=%u PT=%u 后端=%s\n",
                            audio_->receiver_port(), audio_->payload_type(),
                            audio_->backend_name().c_str());
            }
        }
    }
    return true;
}

bool LiveSource::start_playback(std::string &err) {
    if (audio_ == nullptr) {
        err = "没有音频腿可放（--no-audio、起流失败，或这个构建没有音频后端）";
        return false;
    }
    return audio_out_.open(*audio_, err);
}

bool LiveSource::control(double x, double y, bool down, std::string &err) {
    // 手一动就是"接下来画面一定会变"的信号。设备在画面静止时会把流结束掉，而泵
    // 最快也要等满静默窗口才发现——不催这一次，手感就是"点下去愣一下才动"。
    if (pump_ != nullptr) {
        pump_->wake();
    }
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
        std::printf("控制已接通（触摸注入可用）\n");
    }
    return hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, down, err);
}

bool LiveSource::type_text(const std::string &text, int hold_ms, std::string &err) {
    if (hid_ == nullptr) {
        if (hid_unavailable_) {
            return false;
        }
        hid_ = scrctl::hid::Service::open(*device_, err);
        if (hid_ == nullptr) {
            hid_unavailable_ = true;
            return false;
        }
    }
    return hid_->type_text(text, hold_ms, err);
}

bool LiveSource::button(uint16_t usage_page, uint16_t usage_code, std::string &err) {
    if (buttons_ == nullptr) {
        buttons_ = scrctl::hid::Buttons::open(*device_, err);
        if (buttons_ == nullptr) {
            return false;
        }
    }
    return buttons_->press(usage_page, usage_code, 90, err);
}

}  // namespace

int main(int argc, char **argv) {
    // 崩溃时块缓冲的 stdout 会整段丢失，导致"无输出"无法定位。诊断工具
    // 的输出量很小，直接无缓冲。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    if (!parse_args(argc, argv, o)) {
        usage(argv[0]);
        return 2;
    }

    if (o.show_version) {
        // 版本号只有一个来源：CMake 里那个 `project(... VERSION)`。写第二处迟早会对不上，
        // 而"发的二进制里印的版本"是用户报问题时唯一能引用的东西。
        std::printf("scrctl %s\n", SCRCTL_VERSION_STRING);
        return 0;
    }

    if (o.list_devices) {
        std::string err;
        auto devices = scrctl::remote::Device::list(err);
        if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (const auto &d : devices) {
            std::printf("%s  %s\n", d.udid.c_str(), d.connection_type.c_str());
        }
        return 0;
    }

    // 剪贴板是一条独立的路：不需要视频流、不需要窗口，所以放在起流之前，办完就退。
    //
    // 为什么必须有这两条：键盘注入只覆盖 US 布局的 ASCII，中文与 emoji 进不了设备
    // （见 src/remote/Pasteboard.h 的说明）。
    //
    // `--copy` 与 `--paste` 同时给时是"写完立刻读回"，这不是顺手：dtpasteboardd 对
    // 形状不对的内容会**回一个 SET_REPLY 表示收下、然后把内容丢掉**（types 为空就是
    // 这种情况），只发不读的话这种失败在本地完全看不出来。
    if (o.list_apps) {
        std::string err;
        auto dev = scrctl::remote::Device::establish(o.serial, err);
        if (!dev) {
            std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
            return 1;
        }
        std::vector<scrctl::remote::App::Entry> apps;
        if (!scrctl::remote::App::list(*dev, apps, err)) {
            std::fprintf(stderr, "列 App 失败: %s\n", err.c_str());
            return 1;
        }
        for (const auto &e : apps) {
            std::printf("%s\t%s\n", e.bundle_id.c_str(), e.name.c_str());
        }
        std::printf("共 %zu 个\n", apps.size());
        return 0;
    }

    if (!o.copy_text.empty() || o.paste) {
        std::string err;
        auto dev = scrctl::remote::Device::establish(o.serial, err);
        if (!dev) {
            std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
            return 1;
        }
        int rc = 0;
        if (!o.copy_text.empty()) {
            if (scrctl::remote::Pasteboard::set_text(*dev, o.copy_text, err)) {
                std::printf("已写入设备剪贴板：%zu 字节\n", o.copy_text.size());
            } else {
                std::fprintf(stderr, "--copy 写入失败: %s\n", err.c_str());
                rc = 1;
            }
        }
        if (o.paste) {
            std::string text;
            if (scrctl::remote::Pasteboard::get_text(*dev, text, err)) {
                std::printf("设备剪贴板（%zu 字节）：%s\n", text.size(), text.c_str());
            } else {
                std::fprintf(stderr, "--paste 读取失败: %s\n", err.c_str());
                rc = 1;
            }
        }
        return rc;
    }

    std::unique_ptr<FrameSource> source;
    LiveSource *live = nullptr;
    if (!o.path.empty()) {
        source = std::make_unique<FileSource>(o.path);
    } else {
        auto made = std::make_unique<LiveSource>();
        std::string err;
        if (!made->start(o.serial, o.record, o.hw_decode, !o.no_window && o.orientation < 0,
                         !o.no_audio, o.audio_buffer_ms, err)) {
            std::fprintf(stderr, "起流失败: %s\n", err.c_str());
            // 设备在通话中会直接拒绝起流（code 9022）。实测这时它的会话表是空的
            // （getmediastreamserverstatus 回 sessions: []），所以不是"有条旧流占着"，
            // 重试也不会成——不点出来，用户只会以为是我们的流没起来。截图服务不受影响，
            // 受影响的只有这条视频流。
            if (err.find("9022") != std::string::npos) {
                std::fprintf(stderr,
                             "提示：设备正在通话，挂断之后再试即可（不是本地的问题，重起 scrctl 没用）。\n");
            }
            return 1;
        }
        live = made.get();
        source = std::move(made);
    }
    if (live == nullptr && !o.start_app.empty()) {
        std::fprintf(stderr, "--start-app 只对真机实时流有意义（--play 的时候没有设备可启动），已忽略\n");
    }
    if (live != nullptr && !o.start_app.empty()) {
        std::string spec = o.start_app;
        bool by_name = false;
        bool terminate = false;
        // 前缀顺序照 scrcpy：`?` 在前、`+` 在后（它的文档就是按这个顺序写的）。
        while (!spec.empty()) {
            if (spec.front() == '?') {
                by_name = true;
            } else if (spec.front() == '+') {
                terminate = true;
            } else {
                break;
            }
            spec.erase(spec.begin());
        }
        if (spec.empty()) {
            std::fprintf(stderr, "--start-app 的名字是空的\n");
            return 2;
        }
        std::string target = spec;
        if (by_name) {
            // 按名字找要先把整张表拉回来——那是一份几 MB 的回复，所以这一步比按
            // bundle id 慢一个数量级，scrcpy 的文档里也明说了这件事。
            std::string lerr;
            std::vector<scrctl::remote::App::Entry> apps;
            if (!scrctl::remote::App::list(live->device(), apps, lerr)) {
                std::fprintf(stderr, "按名字找 App 失败: %s\n", lerr.c_str());
                return 1;
            }
            auto lower = [](std::string v) {
                for (char &c : v) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return v;
            };
            const std::string want = lower(spec);
            const scrctl::remote::App::Entry *hit = nullptr;
            for (const auto &e : apps) {
                if (lower(e.name).rfind(want, 0) == 0) {
                    hit = &e;
                    break;
                }
            }
            if (hit == nullptr) {
                std::fprintf(stderr, "没有名字以 %s 开头的 App\n", spec.c_str());
                return 1;
            }
            target = hit->bundle_id;
            std::printf("--start-app=?%s -> %s\n", spec.c_str(), target.c_str());
        }
        std::string lerr;
        if (!scrctl::remote::App::launch(live->device(), target, lerr, terminate)) {
            std::fprintf(stderr, "启动 %s 失败: %s\n", target.c_str(), lerr.c_str());
            return 1;
        }
        std::printf("已启动 %s%s\n", target.c_str(), terminate ? "（先杀掉了在跑的实例）" : "");
    }

    const bool control_enabled = live != nullptr && !o.no_control;

    if (!o.render_driver.empty()) {
        // 必须在 SDL_CreateRenderer 之前设；设晚了没有任何提示，只是驱动还是默认那个。
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, o.render_driver.c_str());
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL 初始化失败: %s\n", SDL_GetError());
        return 1;
    }
    // 音频子系统**单独**初始化并且容许失败：一台没有声卡的机器（CI、无 PulseAudio 的
    // Linux、被拔掉的接口设备）上把 SDL_INIT_AUDIO 塞进主 SDL_Init 会让整个镜像起不来，
    // 而"没有声音"从来不是不能镜像的理由。
    if (live != nullptr && !o.no_audio && !o.no_audio_playback &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        std::string aerr;
        if (!live->start_playback(aerr)) {
            std::fprintf(stderr, "打不开音频出口: %s（音频腿照收，只是不出声）\n",
                         aerr.c_str());
        }
    } else if (live != nullptr && live->has_audio() && o.no_audio_playback) {
        std::printf("--no-audio-playback：音频腿在收与解，只是不在本机放\n");
    }
    // 必须在 SDL_Init **之后**：signal() 是抢椅子，谁最后装谁说了算，先装会被它盖掉
    // （而 sdl2-compat 没有提供 SDL_HINT_NO_SIGNALS 可以让它别接）。
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    if (o.disable_screensaver) {
        SDL_DisableScreenSaver();
    }

    // 输入通路的无头自检：注入一条直线就退出。用直线而不是点一下，是因为
    // "画布被拖走一段"在截图上可判定，而一次点击在多数应用里没有可见后果。
    if (live != nullptr && !o.test_touch.empty()) {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (std::sscanf(o.test_touch.c_str(), "%f,%f,%f,%f", &x0, &y0, &x1, &y1) != 4) {
            std::fprintf(stderr, "--test-touch 格式应为 X0,Y0,X1,Y1，收到 %s\n",
                         o.test_touch.c_str());
            return 2;
        }
        std::string cerr;
        bool ok = true;
        for (int i = 0; i <= 20 && ok; ++i) {
            const double t = i / 20.0;
            ok = live->control(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, i < 20, cerr);
            if (i < 20) {
                SDL_Delay(12);
            }
        }
        std::printf("--test-touch (%.3f,%.3f)->(%.3f,%.3f): %s%s\n", x0, y0, x1, y1,
                    ok ? "已注入" : "失败", ok ? "" : cerr.c_str());
        return ok ? 0 : 1;
    }

    // 按键的判据要另想办法：音量 HUD 只显示一秒多，另起一次截图会话根本来不及。
    // 所以这里只负责"按下去"，看效果交给同一进程里已经在跑的镜像——
    // 配 --verify N 回读第 N 帧，HUD 就在那一帧里。
    if (live != nullptr && !o.test_button.empty()) {
        static const std::pair<const char *, uint16_t> kCodes[] = {
            {"home", scrctl::hid::button::kHome},   {"lock", scrctl::hid::button::kLock},
            {"volup", scrctl::hid::button::kVolumeUp},
            {"voldn", scrctl::hid::button::kVolumeDown}, {"mute", scrctl::hid::button::kMute},
        };
        uint16_t code = 0;
        for (const auto &e : kCodes) {
            if (o.test_button == e.first) {
                code = e.second;
                break;
            }
        }
        if (code == 0) {
            std::fprintf(stderr, "不认识按键 %s（可用：home/lock/volup/voldn/mute）\n",
                         o.test_button.c_str());
            return 2;
        }
        std::string berr;
        if (live->button(scrctl::hid::button::kUsagePageConsumer, code, berr)) {
            std::printf("--test-button %s: 已按下\n", o.test_button.c_str());
        } else {
            std::fprintf(stderr, "--test-button %s 失败: %s\n", o.test_button.c_str(),
                         berr.c_str());
            return 1;
        }
    }

    if (live != nullptr && !o.test_type.empty()) {
        std::string terr;
        if (live->type_text(o.test_type, 40, terr)) {
            std::printf("--test-type %s: 已注入\n", o.test_type.c_str());
        } else {
            std::fprintf(stderr, "--test-type 失败: %s\n", terr.c_str());
            return 1;
        }
    }

    int rendered = 0;
    std::unique_ptr<Presenter> presenter;
    // 窗口现在按哪一档朝向摆着。-1 = 还没建过窗口，所以第一帧必然进一次重建分支。
    int applied_degrees = -1;
    // 只为少打一行：第一次建窗口不需要喊"旋转 -> 重建"。
    bool first_window = true;
    const Uint64 start = SDL_GetTicks64();
    Uint64 last_stats_at = SDL_GetTicks64();
    bool quit = false;

    /// 窗口里的一次按下/移动/抬起 -> 设备上的接触/抬起。
    ///
    /// 注入失败只打一次：这是本地窗口在动鼠标，失败刷屏会把有用的帧率信息冲掉。
    std::string control_err;
    bool control_warned = false;
    auto on_touch = [&](double x, double y, bool down) {
        if (!control_enabled || live == nullptr) {
            return;
        }
        if (!live->control(x, y, down, control_err) && !control_warned) {
            control_warned = true;
            std::fprintf(stderr, "注入输入失败（已停止尝试）: %s\n", control_err.c_str());
        }
    };

    // 帧缓冲要跨迭代复用：每轮新建一个 Frame 意味着每帧重新申请 11MB、重新缺页，
    // 而取帧那边是 `out = frame_` 的整幅拷贝——两者叠起来实测就是每帧几十毫秒。
    scrctl::Frame f;
    int last_rendered = 0;
    while (!quit) {
        if (o.time_limit > 0 && SDL_GetTicks64() - start >=
                                   static_cast<Uint64>(o.time_limit) * 1000) {
            std::printf("达到 --time-limit %d 秒\n", o.time_limit);
            break;
        }
        if (g_stop_requested.load()) {
            std::printf("收到退出信号，走正常退出路径（要把设备侧那条流停掉）\n");
            break;
        }
        // 读数放在取帧**之前**。以前它挂在"这一轮取到帧了"那条分支里，于是断流的那
        // 几秒恰好是不打印的那几秒——日志在最有信息量的时刻静音，恢复之后又连着几行
        // 看不出为什么掉帧（用户报"有时候会断"，而日志里那一段什么都沒有，只有事后
        // 被拉低的平均帧率）。没帧的时候窗口照样过，打出来就是 0 fps，那才是真相。
        //
        // 按秒催、不按"每 60 帧"（12fps 时每 60 帧是 5 秒，读数摊在很长的窗口上，
        // 速率和累计值分不出来）；而且**必须打本段的速率**：`渲染 N 帧` 那一路历史上
        // 打的是"总数 / 全程时间"，一次 3 秒的停顿会把平均帧率压到 47，之后每一行都
        // 显示 47.3、47.6、47.9、48.2，看起来像"恢复之后还在持续掉帧"——而实测那几段
        // 的瞬时值是 55、55、56，早就好了。平均数只能用来发现"一直在掉"，不能用来
        // 判断"现在在掉"。
        if (o.stats && SDL_GetTicks64() - last_stats_at >= 1000) {
            const Uint64 at = SDL_GetTicks64();
            const double win = std::max(0.001, static_cast<double>(at - last_stats_at) / 1000.0);
            const int got = rendered - last_rendered;
            std::printf("  渲染 %d 帧（本段 %d 帧 = %.1f fps，全程均 %.1f fps）\n", rendered, got,
                        got / win,
                        rendered / std::max(0.001, static_cast<double>(at - start) / 1000.0));
            source->print_stats();
            last_stats_at = at;
            last_rendered = rendered;
        }

        // 50ms：再长一点，等帧期间窗口对关闭/移动的反应就开始发木。
        if (!source->next(f, 50)) {
            if (source->finished()) {
                std::printf("源已结束\n");
                break;
            }
            // 没帧可画也要让窗口活着——此刻基本都是在等下一帧到达，而等帧的时候
            // 不泵事件，窗口就是"未响应"。
            if (presenter != nullptr) {
                quit = presenter->pump(on_touch);
            }
            continue;
        }

        if (o.no_window) {
            // 无窗口模式：只消费帧不画。给脚本/自动化用（Maa 那条路就不要窗口）。
            ++rendered;
            if (o.exit_after > 0 && rendered >= o.exit_after) {
                std::printf("达到 --exit-after %d\n", o.exit_after);
                break;
            }
            continue;
        }

        // 朝向变了就把整个 Presenter 重建一次，而不是原地改窗口尺寸。
        //
        // 为什么重建：原地改要 `SDL_SetWindowSize` + 重设 logical size，而实测在
        // dummy 驱动下绘制面尺寸根本不跟着窗口变（render_test 就是这么抓到的）。
        // 真驱动大概率跟得上，但"大概率"在这儿不够——尺寸不跟着变时 SDL 会按新的
        // logical size 等比留边，画面缩在窗口一角，那正是我们要修的 bug 的新版本。
        // 重建走的是启动时那条已经验过的路，代价只是窗口闪一下（转屏本来就是一个
        // 动作，不是每帧的事）。
        const int degrees = o.orientation >= 0 ? o.orientation : source->orientation_degrees();
        if (degrees != applied_degrees) {
            applied_degrees = degrees;
            presenter.reset();
            presenter = std::make_unique<Presenter>();
            presenter->set_debug_input(o.debug_input);
            WindowSpec spec;
            spec.title = o.title;
            spec.want_w = o.win_w;
            spec.want_h = o.win_h;
            spec.x = o.win_x;
            spec.y = o.win_y;
            spec.always_on_top = o.always_on_top;
            spec.borderless = o.borderless;
            spec.fullscreen = o.fullscreen;
            spec.want_readback = o.verify_at > 0;
            presenter->set_background(o.bg[0], o.bg[1], o.bg[2]);
            if (!presenter->open(static_cast<int>(f.width), static_cast<int>(f.height),
                                 resolve_crop(o, f, *source), degrees, o.scale, o.scale_given,
                                 spec)) {
                return 1;
            }
            if (first_window) {
                first_window = false;
            } else {
                std::printf("界面旋转 -> 顺时针 %d°，窗口按新朝向重建\n", degrees);
            }
        }

        const bool do_verify = o.verify_at > 0 && rendered + 1 == o.verify_at;
        presenter->draw(f, do_verify ? o.verify_path.c_str() : nullptr);
        ++rendered;

        // 只有文件回放需要自己按标称帧率追节拍；实时流的到达节奏就是设备的节奏。
        if (source->paces_itself()) {
            const Uint64 want_ms = static_cast<Uint64>(rendered) * 1000 / 60;
            const Uint64 now = SDL_GetTicks64() - start;
            if (want_ms > now) {
                SDL_Delay(static_cast<Uint32>(want_ms - now));
            }
        }

        if (o.exit_after > 0 && rendered >= o.exit_after) {
            std::printf("达到 --exit-after %d\n", o.exit_after);
            break;
        }
        quit = presenter->pump(on_touch);
    }

    std::printf("完成：渲染 %d 帧\n", rendered);
    presenter.reset();
    // 关声卡必须在 SDL_Quit 之前：Quit 把音频子系统拆了之后再去
    // SDL_CloseAudioDevice，操作的就是一个已经不存在的上下文。
    if (live != nullptr) {
        live->stop_playback();
    }
    SDL_Quit();
    return 0;
}
