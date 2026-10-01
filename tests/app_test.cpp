// launchapplication 请求体的离线自检。
//
// 为什么必须有：这条 RPC 的键放错位置时，设备**不给字段级报错**，而是退到别的分支
// 说一句无关的话。实测把 bundle id 塞进 options，回的是 "A URL to open must be
// specified in the launch options."——照着这句话去找"该给哪个 url 键"会一路找错，
// 因为真正缺的是顶层的 applicationSpecifier。所以形状只能钉在这里。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/Reap.h"
#include "app/SourcePick.h"
#include "app/StatsWindow.h"
#include "plist/Plist.h"
#include "remote/App.h"
#include "xpc/XpcValue.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

}  // namespace

int main() {
    using scrctl::remote::App;
    using namespace scrctl::xpc;

    constexpr const char *kBundle = "com.apple.mobilesafari";
    auto input = App::build_launch(kBundle, /*terminate_existing = */ true);

    std::printf("== bundle id 的入口 ==\n");
    const auto *spec = input.find("applicationSpecifier");
    check(spec != nullptr && spec->is_dict(), "顶层有 applicationSpecifier 字典");
    const auto *bid = spec == nullptr ? nullptr : spec->find("bundleIdentifier");
    check(bid != nullptr && bid->is_dict(), "  里面是 bundleIdentifier case");
    const auto *zero = bid == nullptr ? nullptr : bid->find("_0");
    // 带关联值的枚举 case 就是 {_0: 值}，少这一层设备认不出这个 specifier。
    check(zero != nullptr && zero->as_string_or() == kBundle, "  _0 就是 bundle id");
    // 反过来说：options 里出现任何"看起来像指定要启动什么"的键，都是走回头路。
    const auto *options = input.find("options");
    check(options != nullptr && options->is_dict(), "options 是字典");
    if (options != nullptr) {
        for (const char *wrong : {"bundleIdentifier", "url", "applicationURL", "path"}) {
            check(options->find(wrong) == nullptr,
                  std::string("options 里没有 ") + wrong + "（那个位置不生效）");
        }
    }

    std::printf("== options 的必填项 ==\n");
    check(options != nullptr && options->find("terminateExisting") != nullptr &&
                  options->at("terminateExisting").as_bool_or(),
          "terminateExisting 跟着参数走（true = 先杀掉在跑的实例）");
    if (const auto *user = options == nullptr ? nullptr : options->find("user");
        user != nullptr) {
        check(user->at("shortName").as_string_or() == "mobile", "user.shortName=mobile");
    } else {
        check(false, "有 user 字典");
    }
    // 这条是实测换来的：零长 Data 被拒（"Cannot parse a NULL or zero-length data"），
    // 所以哪怕内容什么都没有，也得是一段解析得动的 plist。
    const auto *pso = options == nullptr ? nullptr : options->find("platformSpecificOptions");
    check(pso != nullptr && pso->type == Type::Data && !pso->data.empty(),
          "platformSpecificOptions 是非空 Data");
    if (pso != nullptr && !pso->data.empty()) {
        const std::string text(pso->data.begin(), pso->data.end());
        std::string perr;
        const auto parsed = scrctl::plist::parse(text, &perr);
        check(parsed.has_value() && parsed->is_dict(), "  而且是一段能解开的 plist 字典");
    }
    check(options != nullptr && options->at("arguments").is_array(), "arguments 是数组");
    check(options != nullptr && options->at("environmentVariables").is_dict(),
          "environmentVariables 是字典");
    check(input.find("standardIOIdentifiers") != nullptr &&
                  input.at("standardIOIdentifiers").is_dict(),
          "standardIOIdentifiers 是顶层字典（不在 options 里）");

    std::printf("== 参数化 ==\n");
    const auto keep = App::build_launch(kBundle, /*terminate_existing = */ false);
    check(!keep.at("options").at("terminateExisting").as_bool_or(true),
          "terminate_existing=false 时该项为假（与 Android 的 monkey -p 语义对齐）");

    std::printf("== 进程归属：bundle 目录 -> pid ==\n");
    // stop_app 的全部风险都在这条匹配上：匹配不到就"返回真而 App 还活着"，匹配多了
    // 就杀错进程。而这两种在线上都没有任何痕迹。样本形状取自真机 listprocesses。
    auto proc = [](int64_t pid, const char *url) {
        auto relative = make_dict();
        dict_set(relative, "relative", make_string(url));
        auto exe = make_dict();
        dict_set(exe, "executableURL", std::move(relative));
        dict_set(exe, "processIdentifier", make_int64(pid));
        return exe;
    };
    constexpr const char *kAppPath =
        "/private/var/containers/Bundle/Application/F256/MobileSafari.app";
    auto tokens = make_array();
    // 主进程
    array_push(tokens, proc(24368, "file:///private/var/containers/Bundle/Application/"
                                         "F256/MobileSafari.app/MobileSafari"));
    // 同 App 的扩展进程，也该算进来
    array_push(tokens, proc(24370, "file:///private/var/containers/Bundle/Application/"
                                         "F256/MobileSafari.app/PlugIns/Ext.appex/Ext"));
    // 只差一个字符的另一个 App——没有末尾那个 '/' 就会被前缀误伤
    array_push(tokens, proc(24369, "file:///private/var/containers/Bundle/Application/"
                                         "F256/MobileSafari.app2/MobileSafari"));
    // 系统守护进程
    array_push(tokens, proc(100, "file:///usr/libexec/wifianalyticsd"));
    auto processes = make_dict();
    dict_set(processes, "processTokens", std::move(tokens));

    const auto pids = App::matching_pids(processes, kAppPath);
    check(pids.size() == 2, "只认出属于这个 App 的两个进程");
    check(!pids.empty() && pids[0] == 24368 && pids[1] == 24370,
          "pid 与顺序都对，且 .app2 那种近似名没被误伤");
    check(App::matching_pids(processes, "").empty(), "空路径不匹配任何东西（防误杀全部）");

    // 运行中降级那本状态账。第一版写在 next() 里时把"起流就降级"（泵不存在）判成了
    // "回升"，而那一格在 iOS 18 上是常态；第二版把截图源的一次失败判成永久，冷却这格
    // 就是为它加的（审查 P2）。组合在这里跑全，不等真机凑现场。
    using scrctl::app::pick_picture_source;
    using scrctl::app::SourcePick;
    constexpr uint64_t kNever = UINT64_MAX;
    check(pick_picture_source(true, false, false, kNever) == SourcePick::kStayStream,
          "正常：等媒体泵");
    check(pick_picture_source(true, true, false, kNever) == SourcePick::kToShot,
          "跑着解不出画面：切去截图源");
    check(pick_picture_source(true, true, false, 0) == SourcePick::kStayStream,
          "截图源刚起失败：冷却期内不撞");
    check(pick_picture_source(true, true, false, scrctl::app::kShotRetryMs - 1) ==
              SourcePick::kStayStream,
          "冷却期差一毫秒也还不撞");
    check(pick_picture_source(true, true, false, scrctl::app::kShotRetryMs) ==
              SourcePick::kToShot,
          "冷却一过就再试：一次失败不判永久");
    check(pick_picture_source(true, true, true, kNever) == SourcePick::kStayShot,
          "已降级且泵仍解不出：留在截图路");
    check(pick_picture_source(true, false, true, kNever) == SourcePick::kToStream,
          "泵回升：切回实时流");
    check(pick_picture_source(false, false, true, kNever) == SourcePick::kStayShot,
          "起流就降级（泵不存在）：留在截图路——第一版写错的就是这一格");
    check(pick_picture_source(false, false, true, 0) == SourcePick::kStayShot,
          "起流就降级且截图源也曾失败：它活着就继续用它");
    check(pick_picture_source(false, false, false, kNever) == SourcePick::kStayStream,
          "两条路都没有：交给调用方判空，不在这里编一个来源");

    // 退役截图源的回收规则（审查 P2，第四轮）。切回实时流时不能 join（渲染线程会被一次
    // 截图 RPC 冻到上限），退下来的源先挂着；但每个都揣着一整张解码好的 BGRA 画面，
    // 挂到 teardown 就是按切换次数堆内存。真机上打不响"反复降级"这个触发器，所以规则
    // 离线跑全。两条判据缺一不可：**没退的绝不能被销毁**（那次析构里的 join 就是等待，
    // 等于把 P3 又请回来），**退了的要当场就销毁**（否则内存还是没还）。
    {
        struct Fake {
            Fake(int id, bool done, int &destroyed)
                : id_(id), done_(done), destroyed_(destroyed) {}
            ~Fake() { ++destroyed_; }
            [[nodiscard]] bool worker_done() const { return done_; }
            int id_;
            bool done_;
            int &destroyed_;
        };
        const auto reap = [](std::vector<std::unique_ptr<Fake>> &v) {
            return scrctl::app::reap_finished(v, [](const Fake &f) { return f.worker_done(); });
        };
        const auto ids = [](const std::vector<std::unique_ptr<Fake>> &v) {
            std::string s;
            for (const auto &p : v) {
                s += std::to_string(p->id_);
            }
            return s;
        };
        int destroyed = 0;
        std::vector<std::unique_ptr<Fake>> retired;
        // 按 id 标记而不是按下标：这条规则一旦回归成"不看 done 全销毁"，表就空了，
        // 按下标写会先越界把测试自己搞崩（实测 exit 139），而崩溃比一行 FAIL 难读得多。
        const auto mark_done = [&retired](int id) {
            for (const auto &p : retired) {
                if (p->id_ == id) {
                    p->done_ = true;
                }
            }
        };

        check(reap(retired) == 0 && retired.empty() && destroyed == 0, "空表：摘 0 个，不炸");

        retired.emplace_back(std::make_unique<Fake>(1, false, destroyed));
        retired.emplace_back(std::make_unique<Fake>(2, false, destroyed));
        retired.emplace_back(std::make_unique<Fake>(3, false, destroyed));
        check(reap(retired) == 0 && retired.size() == 3 && destroyed == 0,
              "worker 都还没退：一个都不摘，也一个都不销毁");

        mark_done(2);
        check(reap(retired) == 1 && ids(retired) == "13" && destroyed == 1,
              "中间那个退了：只摘它，剩下的顺序不变，而且摘掉就是真销毁了");

        mark_done(1);
        mark_done(3);
        check(reap(retired) == 2 && retired.empty() && destroyed == 3, "都退了：清空");
        check(reap(retired) == 0 && destroyed == 3, "再跑一趟幂等（不会重复销毁）");
    }

    // `--test-degrade` 那本时刻表。它存在的理由是真机上打不响"跑着跑着解不出画面"，而
    // 连着三轮审查的修复全在那一格上；所以这条旗标自己的判据必须先离线钉死——一个把
    // "放开"判成"强制"的开关，会让人对着日志读出完全反的结论。
    {
        const std::vector<uint64_t> none;
        check(!scrctl::app::degrade_forced(0, none) &&
                  !scrctl::app::degrade_forced(999999, none),
              "没给时刻表：永远不强制（产品路径就是这一格）");

        std::vector<uint64_t> marks;
        std::string perr;
        check(scrctl::app::parse_degrade_marks("4,8,12", marks, perr) && marks.size() == 3 &&
                  marks[0] == 4000 && marks[1] == 8000 && marks[2] == 12000,
              "\"4,8,12\" -> 4000/8000/12000 毫秒: " + perr);
        check(scrctl::app::parse_degrade_marks("0.5", marks, perr) && marks.size() == 1 &&
                  marks[0] == 500,
              "小数秒也认（0.5 -> 500 毫秒）: " + perr);

        scrctl::app::parse_degrade_marks("4,8,12", marks, perr);
        check(!scrctl::app::degrade_forced(3999, marks), "第一个时刻之前不强制");
        check(scrctl::app::degrade_forced(4000, marks), "到点即强制（含边界那一毫秒）");
        check(scrctl::app::degrade_forced(7999, marks), "段内一直是强制");
        check(!scrctl::app::degrade_forced(8000, marks), "第二个时刻起放开");
        check(scrctl::app::degrade_forced(12000, marks), "第三个时刻再强制：来回切靠的是交替");
        check(scrctl::app::degrade_forced(UINT64_MAX, marks),
              "最后一个时刻之后停在强制段（奇数个时刻的语义就是\"切过去不再回来\"）");

        check(!scrctl::app::parse_degrade_marks("", marks, perr) && !perr.empty(),
              "空规格要报错而不是当成\"没有时刻\": " + perr);
        check(!scrctl::app::parse_degrade_marks("8,4", marks, perr), "降序要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("4,,8", marks, perr), "中间空一段要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("4,", marks, perr), "尾巴上多个逗号要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("4x", marks, perr), "认不出的字符要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("-1", marks, perr), "负数要报错: " + perr);

        // 非有限数与装不下的数（审查 P2，第五轮）。`strtod` 认 nan/inf，也认 "1e400"
        // （溢出成 inf），而解析器下一步就 `static_cast<uint64_t>(secs * 1000.0)`——
        // **从 NaN 或超出目标类型的浮点值转整数是未定义行为**，UBSan 会报 runtime
        // error。负数那一关拦不住它们：nan 与任何数比较都是 false。
        // 这条旗标的用途是打判据，一个静默变成天文数字（或 0）的时刻表比直接报错更糟：
        // 它会让人对着一个从没按预期生效的开关读日志。
        check(!scrctl::app::parse_degrade_marks("nan", marks, perr), "nan 要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("inf", marks, perr), "inf 要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("-inf", marks, perr),
              "-inf 要报错（这一条今天靠负数那关就拦住了，留着当回归）: " + perr);
        check(!scrctl::app::parse_degrade_marks("4,nan", marks, perr),
              "夹在中间的非有限数也要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("1e400", marks, perr),
              "strtod 溢出成 inf 也要报错: " + perr);
        check(!scrctl::app::parse_degrade_marks("1e18", marks, perr),
              "乘 1000 之后装不进 uint64 的要报错: " + perr);
        // 反面：装得下的大数仍然要认。别把"大"当成错——它只是永远到不了那一刻，
        // 而多设一个上限就等于多一条要解释的规矩。
        check(scrctl::app::parse_degrade_marks("1e9", marks, perr) && marks.size() == 1,
              "1e9 秒（换算成毫秒装得下）照收: " + perr);
    }

    // 一本账一把尺（审查 P2，第五轮）。场景是真机上跑过的那个形状：媒体流解不出画面
    // → 降级到截图兜底 30 秒 → 泵回升、切回实时流。兜底那 30 秒里**媒体泵仍在后台收包**
    // （音频腿也照收照解），只是 print_stats 走的是截图分支、只结算截图那本账。
    //
    // 这段同时跑两种接法，把"共用一把尺"当负对照钉在这里：共用尺被截图分支每秒推一次，
    // 于是切回来那一段的分母只剩约 1 秒，3100 个包的增量被读成 3100/s（真实 100/s）；
    // 分账尺的分母是整段 31 秒，读数回到 100/s。**哪天有人把三把尺合回一把，这里的
    // 断言会先红给他看**——那条路的读数长什么样，不必再等真机重现一次。
    {
        using scrctl::app::counter_delta;
        using scrctl::app::settle_window;
        const uint64_t kPerSec = 100;  // 泵的收包速率（实测空闲时也是 100 包/秒量级）
        const uint64_t t0 = 1000000;   // 非零起点：0 是"还没起表"的哨兵
        uint64_t packets = 0;          // 泵的累计收包，全程只增，兜底期间照涨

        uint64_t shared_ms = t0;  // 旧接法：两个分支共用一把尺
        uint64_t stream_ms = t0;  // 新接法：媒体那本账自己的尺
        uint64_t base_shared = 0;
        uint64_t base_split = 0;
        auto settle_both = [&](uint64_t t, double &shared_rate, double &split_rate) {
            const uint64_t d1 = counter_delta(packets, base_shared);
            const double s1 = settle_window(t, shared_ms);
            shared_rate = static_cast<double>(d1) / s1;
            const uint64_t d2 = counter_delta(packets, base_split);
            const double s2 = settle_window(t, stream_ms);
            split_rate = static_cast<double>(d2) / s2;
        };

        double shared_rate = 0, split_rate = 0;
        // 实时流上跑 3 秒：两种接法都该读出真实速率
        for (uint64_t t = t0 + 1000; t <= t0 + 3000; t += 1000) {
            packets += kPerSec;
            settle_both(t, shared_rate, split_rate);
        }
        check(shared_rate == 100.0 && split_rate == 100.0,
              "实时流上每秒结算：两种接法都读 100/s");

        // 降级到截图兜底 30 秒：print_stats 走截图分支，只有共用尺被推
        uint64_t shot_ms = t0 + 3000;  // 截图那本账的尺，起流就降级那一刻起表
        for (uint64_t t = t0 + 4000; t <= t0 + 33000; t += 1000) {
            packets += kPerSec;
            (void)settle_window(t, shot_ms);  // 截图那本账自己结算（张数那一路，这里不关心）
            (void)settle_window(t, shared_ms);  // 旧接法：同一把尺被截图分支推掉
        }
        // 泵回升，切回实时流：媒体那本账第一次结算
        packets += kPerSec;
        settle_both(t0 + 34000, shared_rate, split_rate);
        check(split_rate == 100.0, "分账尺：切回来那一段仍是 100/s（分母是整段 31 秒）");
        check(shared_rate == 3100.0,
              "负对照：共用尺把那 3100 个包除以约 1 秒，读出 3100/s——虚高 31 倍");

        // 尺本身的三条边界
        uint64_t fresh = 0;
        check(settle_window(t0 + 5000, fresh) == 1.0 && fresh == t0 + 5000,
              "没起表的尺：第一段回退成 1.0 秒并就此起表（正常路径不该走到，源建好时就起表）");
        uint64_t same = t0;
        settle_window(t0, same);
        check(settle_window(t0, same) == 0.001, "同一毫秒内结算两次：分母下限 0.001，不会除零");
        uint64_t base = 100;
        check(counter_delta(5, base) == 0 && base == 5,
              "累计数倒退（换源/重起会话会从 0 重数）按 0 算，不下溢成天文数字");
    }

    std::printf(Failures == 0 ? "\n全部通过\n" : "\n%d 项失败\n", Failures);
    return Failures == 0 ? 0 : 1;
}
