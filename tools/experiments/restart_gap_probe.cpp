// 比较不同停止条件和等待间隔下，下一次 startmediastream 的结果。
//
// 每条原始会话显式请求 RTCP 空闲超时且不发 RR；--lease 默认为 20 秒。
// 这不是固定寿命保证。自然结束必须由会话表确认，未确认就不能作为“结束后重启”样本。
// --gaps 控制释放本地主机会话对象之后、发送下一次起流请求之前的间隔；
// 起流 RPC 延迟单独测量，不把人为等待计入 RPC 耗时。
#include "ProbeCli.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "ProbeWait.h"
#include "media/StreamSession.h"
#include "remote/Device.h"

namespace {
bool parse_list(std::string_view text, std::vector<int> &out) {
    if (text.empty()) { return false; }
    for (;;) {
        const auto comma = text.find(',');
        auto item = text.substr(0, comma);
        int value = 0;
        if (!scrctl::probe::parse_decimal_integer(item, value, 0, std::numeric_limits<int>::max())) {
            return false;
        }
        out.push_back(value);
        if (comma == std::string_view::npos) { return true; }
        text.remove_prefix(comma + 1);
        if (text.empty()) { return false; }
    }
}

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int trials = 3, lease = 20, wait_ms = 35000;
    std::string gap_spec = "0,250,500,1000,2000";
    bool verbose = false, dry_run = false;
    CLI::App app("比较停止条件与等待间隔对下一次起流的影响");
    app.add_option("--gaps", gap_spec, "逗号分隔的等待毫秒数（默认 0,250,500,1000,2000）");
    app.add_option("--trials", trials, "每组重复次数（默认 3）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--lease", lease, "请求的 RTCP 空闲超时，秒（默认 20，不发送 RR）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_option("--wait-ms", wait_ms, "自然结束观察预算，毫秒（默认 35000；不含 RPC 与清理时限）")->transform(scrctl::probe::decimal_integer(1, std::numeric_limits<int>::max()));
    app.add_flag("-v,--verbose", verbose, "输出协议日志");
    app.add_flag("--dry-run", dry_run, "只显示实验参数，不连接设备");
    app.footer("需独占设备媒体服务：stopmediastream 使用 stopAll，会结束设备上的其它媒体会话。\n观察预算不包含连接、RPC 和停止媒体各自的协议时限。");
    try { app.parse(argc, argv); }
    catch (const CLI::CallForHelp &e) { return app.exit(e); }
    catch (const CLI::ParseError &e) { std::fprintf(stderr, "参数错误：%s\n", e.what()); return 2; }
    std::vector<int> gaps;
    if (!parse_list(gap_spec, gaps)) {
        std::fprintf(stderr, "--gaps 应为逗号分隔的完整十进制整数，范围 0..2147483647，不能有空项\n");
        return 2;
    }
    std::printf("实验参数：timeout=%d 秒，RR=off，trials=%d，wait_ms=%d，gaps=", lease, trials, wait_ms);
    for (size_t i = 0; i < gaps.size(); ++i) { std::printf("%s%d", i ? "," : "", gaps[i]); }
    std::printf("\n");
    if (dry_run) { return 0; }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) { std::fprintf(stderr, "建立会话失败: %s\n", err.c_str()); return 1; }
    struct Row {
        const char *what;
        int gap_ms;
        long cost_ms = 0;
        bool started = false;
        std::string error {};
    };
    std::vector<Row> rows;
    bool any_start_failure = false;
    auto trial = [&](Row &row, bool wait_ended, bool call_stop) {
        scrctl::media::StreamSession::Request req;
        req.timeout_seconds = static_cast<uint32_t>(lease);
        auto session = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        if (!session) { std::fprintf(stderr, "预热起流失败，本样本无效: %s\n", err.c_str()); return false; }
        if (wait_ended) {
            scrctl::probe::Deadline wait(now_ms(), wait_ms);
            bool ended = false;
            uint64_t next_poll = now_ms();
            std::string last_error;
            while (!wait.expired(now_ms())) {
                // 即使媒体持续到达，也要按总截止退出，并定期查询设备状态。
                std::vector<uint8_t> packet;
                std::string receive_error;
                session->next_packet(packet, static_cast<int>(std::min<uint64_t>(100, wait.remaining(now_ms()))), receive_error);
                if (now_ms() >= next_poll && !wait.expired(now_ms())) {
                    next_poll = now_ms() + 1000;
                    const auto state = scrctl::media::StreamSession::probe(*dev, session->started().session_uuid, last_error, verbose);
                    if (!wait.expired(now_ms()) && state == scrctl::media::StreamSession::ServerState::Ended) { ended = true; break; }
                }
            }
            if (!ended) {
                std::fprintf(stderr, "%dms 观察预算内未确认会话结束；不执行后续起流，不生成结束后重启样本。%s\n",
                             wait_ms, last_error.c_str());
                std::string cleanup_error;
                session->stop(*dev, cleanup_error, verbose);
                return false;
            }
        }
        if (call_stop && !session->stop(*dev, err, verbose)) {
            std::fprintf(stderr, "停止会话失败，本样本无效: %s\n", err.c_str());
            return false;
        }
        // StreamSession 析构只关闭本地对象，不发送 stop；第三个实验臂依赖已确认自然结束。
        session.reset();
        const uint64_t gap_start = now_ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(row.gap_ms));
        const uint64_t t0 = now_ms();
        auto second = scrctl::media::StreamSession::start(*dev, req, err, verbose);
        row.cost_ms = static_cast<long>(now_ms() - t0);
        if (!second) {
            row.error = err;
            any_start_failure = true;
            std::fprintf(stderr, "%s，gap=%dms：起流失败，RPC %ldms: %s\n", row.what, row.gap_ms, row.cost_ms, err.c_str());
            // 前提已经成立，这次 RPC 失败是间隔扫描要记录的结果。允许下一独立样本继续，
            // 但最终返回失败；冷却只发生在下一次预热之前，不计入本次 gap。
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return true;
        }
        row.started = true;
        std::printf("  %s，gap=%dms：RPC %ldms，等待与 RPC 合计 %llums\n", row.what, row.gap_ms,
                    row.cost_ms, static_cast<unsigned long long>(now_ms() - gap_start));
        if (!second->stop(*dev, err, verbose)) { std::fprintf(stderr, "清理新会话失败: %s\n", err.c_str()); return false; }
        return true;
    };
    for (int t = 0; t < trials; ++t) {
        for (int gap : gaps) {
            std::printf("\n[样本 %d，gap=%dms]\n", t + 1, gap);
            rows.push_back({"主动停止后再起", gap});
            if (!trial(rows.back(), false, true)) { return 1; }
            rows.push_back({"确认自然结束后发送 stop 再起", gap});
            if (!trial(rows.back(), true, true)) { return 1; }
            rows.push_back({"确认自然结束后直接再起", gap});
            if (!trial(rows.back(), true, false)) { return 1; }
        }
    }
    std::printf("\n完成 %zu 个前提有效的样本；结果仅描述本次设备与配置，不推断通用最小起流间隔。\n", rows.size());
    for (const auto &row : rows) {
        std::printf("  %s gap=%dms：%s，RPC=%ldms %s\n", row.what, row.gap_ms,
                    row.started ? "成功" : "失败", row.cost_ms, row.error.c_str());
    }
    return any_start_failure ? 1 : 0;
}
