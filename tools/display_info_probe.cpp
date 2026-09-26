// 探针：只订阅 `displayinfoupdates`，把设备推来的显示几何原样打出来。
//
// 为什么单独一个工具、而且**不起视频流**：要回答的是"设备到底给不给我们画界面需要的
// 那套数（尺寸、缩放、朝向、刷新率），以及它在什么时刻推"。这两件事都不需要媒体会话，
// 而把媒体会话搅进来会带来两个噪声：同一台设备同时只容得下一条流（会跟别人互相顶，
// docs §13），以及起流本身就会改变显示状态。
//
// 打印用的是 `xpc::describe(v, SIZE_MAX)`。默认那份 400 字符的截断是给服务目录那种
// "一眼扫过"的场合的，而这条推送的字段正好排在截断点之后——第一次试的时候整条
// `displays[]` 被切成 `...`，等于白跑一趟。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "remote/Device.h"
#include "remote/DisplayInfo.h"
#include "remote/Rsd.h"
#include "xpc/XpcValue.h"

namespace {

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 订阅消息体：`{actualInput:{}, streamProxy:{sideChannel:<客户端 UUID>}}`。
/// 形状与 streamapplist 那条一样（设备侧是 CoreDeviceUtilities 的 StreamingAction.swift）。
scrctl::xpc::Value make_input() {
    std::vector<uint8_t> side(16);
    for (auto &b : side) {
        b = static_cast<uint8_t>(std::random_device {} ());
    }
    auto proxy = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(proxy, "sideChannel",
                          scrctl::xpc::make_uuid(std::span<const uint8_t>(side)));
    auto input = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(input, "actualInput", scrctl::xpc::make_dict());
    scrctl::xpc::dict_set(input, "streamProxy", std::move(proxy));
    return input;
}


const char *type_name(scrctl::xpc::Type t) {
    using scrctl::xpc::Type;
    switch (t) {
        case Type::Null: return "Null";
        case Type::Bool: return "Bool";
        case Type::Int64: return "Int64";
        case Type::UInt64: return "UInt64";
        case Type::Double: return "Double";
        case Type::Date: return "Date";
        case Type::Data: return "Data";
        case Type::String: return "String";
        case Type::Uuid: return "Uuid";
        case Type::Array: return "Array";
        case Type::Dict: return "Dict";
        case Type::FileTransfer: return "FileTransfer";
    }
    return "?";
}

/// 把每个叶子按 `键路径 : 类型` 打出来。
///
/// 为什么要单独打类型：`describe` 里 `1125` 与 `1125.0` **长得一模一样**，而解析器是
/// 按类型取值的。本轮真机上"问不到尺寸"就是因为尺寸那一档其实是浮点（CG 的 CGFloat
/// 是 double），按整数取就静默拿不到。类型只能问机器，不能看打印出来的样子猜。
void dump_types(const scrctl::xpc::Value &v, const std::string &path, int depth) {
    using scrctl::xpc::Type;
    if (depth > 4) {
        return;
    }
    if (v.type == Type::Dict) {
        for (const auto &kv : v.dict) {
            dump_types(kv.value, path + "." + kv.key, depth + 1);
        }
        return;
    }
    if (v.type == Type::Array) {
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            dump_types(v.array[i], path + "[" + std::to_string(i) + "]", depth + 1);
        }
        return;
    }
    std::printf("  %-58s : %s\n", path.c_str(), type_name(v.type));
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 60;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("设备：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    auto conn = dev->connect("com.apple.coredevice.deviceinfo", err, verbose);
    if (conn == nullptr) {
        std::fprintf(stderr, "连 deviceinfo 失败: %s\n", err.c_str());
        return 1;
    }

    const uint64_t t0 = now_ms();
    const uint64_t until = t0 + uint64_t(seconds) * 1000;
    std::printf("已订阅，观察 %d 秒。期间转屏 / 锁屏 / 接外接屏，看它在哪些时刻推。\n", seconds);

    int pushes = 0;
    int resubscribes = 0;
    const auto input = make_input();
    while (now_ms() < until) {
        const int hold_ms = int(until - now_ms()) + 1000;
        std::string e;
        const auto r = conn->stream("com.apple.coredevice.feature.displayinfoupdates", "", input,
                                    [&](const scrctl::xpc::Value &one) {
                                        ++pushes;
                                        std::printf("  #%d +%llums：%s\n", pushes,
                                                    static_cast<unsigned long long>(now_ms() - t0),
                                                    scrctl::xpc::describe(one, ~std::size_t { 0 })
                                                        .c_str());
                                        if (pushes == 1) {
                                            // 只打第一条：每条的形状一样，打多了是噪声。
                                            std::printf("  -- 字段类型 --\n");
                                            const auto *ds = one.find("displays");
                                            if (ds != nullptr && !ds->array.empty()) {
                                                dump_types(ds->array[0], "displays[0]", 0);
                                            }
                                        }
                                        return now_ms() < until;
                                    },
                                    hold_ms, e);
        if (r == scrctl::remote::CallResult::Ok) {
            std::printf("  +%llums 设备自己发了 finishStreaming（订阅到此为止）\n",
                        static_cast<unsigned long long>(now_ms() - t0));
            break;
        }
        // 单次要的等待上限给的是"到观察结束"，所以这里的非 Ok 一律是链路或设备端断了。
        // 断了就重订：不重订的话后半程根本没人在听，"设备会不会推第二次"就测不出来了。
        ++resubscribes;
        std::printf("  +%llums 订阅断了（%s），重开一条连接重订\n",
                    static_cast<unsigned long long>(now_ms() - t0), e.c_str());
        conn = dev->connect("com.apple.coredevice.deviceinfo", e, verbose);
        if (conn == nullptr) {
            std::fprintf(stderr, "重连失败: %s\n", e.c_str());
            return 1;
        }
    }
    std::printf("共收到 %d 次推送，重订 %d 次\n", pushes, resubscribes);

    // 再用**产品那条路**走一遍：上面打的是原始 element，这里打的是解析结果。
    // 两者对不上就是解析器的锅，而不是设备换了形状——这次真机上的
    // "问不到尺寸"就是靠这一句定位到 `currentMode.size` 的。
    std::printf("\n== 走产品的 fetch_display_info ==\n");
    std::string ferr;
    const auto got = scrctl::remote::fetch_display_info(*dev, ferr, verbose);
    if (got == std::nullopt) {
        std::printf("  失败：%s\n", ferr.c_str());
    } else {
        std::printf("  解析出 %zu 块屏，设备朝向 %s\n", got->displays.size(),
                    got->device_orientation.c_str());
        for (const auto &d : got->displays) {
            std::printf("  id=%llu primary=%d external=%d 名=%s 尺寸=%dx%d 朝向=%s\n",
                        static_cast<unsigned long long>(d.id), d.primary ? 1 : 0,
                        d.external ? 1 : 0, d.name.c_str(), d.width, d.height,
                        d.orientation.c_str());
        }
        const auto *by_id = got->find(1);
        const auto *by_primary = got->primary();
        std::printf("  find(1)=%s  primary=%s\n",
                    by_id == nullptr ? "没有" : "有",
                    by_primary == nullptr ? "没有" : "有");
    }
    return 0;
}
