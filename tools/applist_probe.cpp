// 探针：拿到"bundle id -> App 在磁盘上的路径"这张表，为了 stop_app。
//
// 为什么要它：`sendsignaltoprocess` 只认 pid，`listprocesses` 只回
// `{processIdentifier, executableURL}`，两边没有共同的键。要把"用户给的 bundle id"
// 落到一个进程上，必须有一张表把 bundle id 对到它的安装路径，而路径正是
// executableURL 的前缀——那张表只能从 App 列表来。
//
// 两条候选路各试一次，别先选再解释：
//   listapps        —— 早先记着"任一 include* 为 true 就 60 秒不回话"。但那台设备上
//                      另一个已知问题是**大回复传不完**（含全部系统 App 的列表就是几 MB），
//                      所以先把所有 include* 关到 false 再试一次：如果小回复能过，
//                      卡住的从来不是这个开关，而是回复尺寸。
//   streamapplist   —— 分批推，每批一小段，天然避开大回复。设备有这个 feature。
//
// 用法：applist_probe [--stream] [--all] [-v] [UDID]
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "remote/App.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace {

constexpr std::string_view kService = "com.apple.coredevice.appservice";

void unused_placeholder() {}
}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string_view udid;
    bool do_list = false;
    std::string grep;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--grep" && i + 1 < argc) {
            grep = argv[++i];
        } else if (a == "--list") {
            do_list = true;
        } else if (a == "-h" || a == "--help") {
            std::printf("用法: %s [--list] [-v] [UDID]\n", argv[0]);
            return 0;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项 %s\n", a.c_str());
            return 2;
        } else {
            udid = a;
        }
    }

    std::string err;
    auto device = scrctl::remote::Device::establish(udid, err, verbose);
    if (!device) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", device->property("ProductType").c_str(),
                device->property("OSVersion").c_str());

    if (!do_list) {
        // 只证明 listprocesses 通、并给出可粘进离线测试的黄金样本。
        scrctl::xpc::Value procs;
        if (!device->feature(kService, "com.apple.coredevice.feature.listprocesses", "",
                             scrctl::xpc::make_dict(), procs, err, verbose, 30000)) {
            std::fprintf(stderr, "listprocesses 失败: %s\n", err.c_str());
            return 1;
        }
        const auto *tokens = procs.find("processTokens");
        if (tokens == nullptr) {
            std::printf("回信: %s\n", scrctl::xpc::describe(procs).substr(0, 400).c_str());
            return 0;
        }
        std::printf("processTokens %zu 个\n", tokens->array.size());
        // --grep 是"某个 App 还在不在跑"的直接问法。stop_app 之后必须能用它确认
        // 进程真的没了——只看前台截图不够（截图看不出后台残留），而默认只印前 3 条
        // 更是什么都看不见。
        int shown = 0;
        for (const auto &t : tokens->array) {
            const std::string line = scrctl::xpc::describe(t);
            if (!grep.empty()) {
                if (line.find(grep) != std::string::npos) {
                    std::printf("  %s\n", line.c_str());
                    ++shown;
                }
            } else if (shown < 3) {
                std::printf("  %s\n", line.c_str());
                ++shown;
            }
        }
        if (!grep.empty()) {
            std::printf("匹配 %s 的进程：%d 个\n", grep.c_str(), shown);
        }
        return 0;
    }

    std::vector<scrctl::remote::App::Entry> apps;
    if (!scrctl::remote::App::list(*device, apps, err, verbose)) {
        std::fprintf(stderr, "App 列表失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("共 %zu 个 App\n", apps.size());
    for (const auto &e : apps) {
        std::printf("  %-52s %-28s %s\n", e.bundle_id.c_str(), e.name.c_str(),
                    e.path.c_str());
    }
    return 0;
}
