// 探针：停掉一条媒体会话之后，隔多久再起一条才起得来。
//
// 为什么单独量：产品里的"重起媒体会话"就是 stop 紧接着 start，而实测把一串
// start/stop 挨着做时，后面的 start 会失败在 "收数据报超时"（设备不回 answer）。
// 把 fir_probe 的五个变体连着跑时，第一个成功、后面四个全死在这个上面；隔 5 秒
// 手动再起一条又完全正常。所以这不是设备坏了，是**停与起之间的最小间隔**没遵守。
//
// 这件事直接决定用户手感：静默到点或用户一动就重起，如果重起本身失败一次，
// 用户看到的就是"点了没反应"，比原来的卡顿更糟。所以要一个明确的毫秒数，
// 而不是"看起来隔一会儿就好了"。
//
// 用法：restart_gap_probe [--gaps 0,250,500,1000,2000] [--trials N] [--verbose]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "media/StreamSession.h"
#include "remote/Device.h"

namespace {

using namespace std::chrono_literals;

std::vector<int> parse_list(const std::string &s) {
    std::vector<int> out;
    std::stringstream ssv(s);
    std::string item;
    while (std::getline(ssv, item, ',')) {
        if (!item.empty()) {
            out.push_back(std::stoi(item));
        }
    }
    return out;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int trials = 3;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--trials" && i + 1 < argc) {
            trials = std::stoi(argv[++i]);
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }

    struct Row {
        const char *what;
        int ok = 0;
        int failed = 0;
        long worst_ms = 0;
        std::string last_err;
    };
    std::vector<Row> rows;

    auto trial = [&](Row &row, bool wait_ended, bool call_stop) {
        scrctl::media::StreamSession::Request req;
        auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        if (!session) {
            std::printf("  预热起流失败，本样本作废: %s\n", err.c_str());
            std::this_thread::sleep_for(3s);
            return;
        }
        std::string serr;
        if (wait_ended) {
            // 等它自己结束：画面静止后设备大约 7 秒就把会话从表里摘掉。
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(1s);
                std::string perr;
                const auto st = scrctl::media::StreamSession::probe(
                    *dev, session->started().session_uuid, perr, verbose);
                if (st == scrctl::media::StreamSession::ServerState::Ended) {
                    break;
                }
            }
        }
        if (call_stop) {
            session->stop(*dev, serr, verbose);
        }
        session.reset();

        const uint64_t t0 = now_ms();
        scrctl::media::StreamSession::Request req2;
        auto second = scrctl::media::StreamSession::start(*dev, req2, err, verbose);
        const long cost = static_cast<long>(now_ms() - t0);
        if (second) {
            ++row.ok;
            row.worst_ms = std::max(row.worst_ms, cost);
            std::printf("  %-28s 起重流成功 %5ld ms\n", row.what, cost);
            std::string s2;
            second->stop(*dev, s2, verbose);
        } else {
            ++row.failed;
            row.last_err = err;
            std::printf("  %-28s 起重流失败 %5ld ms: %s\n", row.what, cost, err.c_str());
            std::this_thread::sleep_for(3s);
        }
    };

    for (int t = 0; t < trials; ++t) {
        std::printf("\n[样本 %d]\n", t + 1);
        rows.push_back(Row {"活着就停，立刻再起"});
        trial(rows.back(), false, true);
        rows.push_back(Row {"等它自己结束，再 stop 起重"});
        trial(rows.back(), true, true);
        rows.push_back(Row {"等它自己结束，不 stop 起重"});
        trial(rows.back(), true, false);
    }

    std::printf("\n==== 起重流在什么情形下会失败 ====\n");
    std::printf("%-30s %-6s %-6s %s\n", "情形", "成功", "失败", "成功最耗时");
    for (const auto &r : rows) {
        std::printf("%-30s %-6d %-6d %-6ld %s\n", r.what, r.ok, r.failed, r.worst_ms,
                    r.failed ? r.last_err.c_str() : "");
    }
    return 0;
}
