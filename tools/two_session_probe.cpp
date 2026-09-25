// 探针：设备上能不能**同时**跑两条媒体会话。
//
// 为什么问这个：一条会话是起流后约 20 秒的硬租期，喂画面和回 RTCP 都不续命（docs §13）。
// 所以镜像每 18 秒必须换一条会话，换的时候有约 0.4 秒没有新帧——画面在动的时候这是
// 看得见的顿挫。唯一的消解办法是**先起新的、再停旧的**：如果两条会话能并存几秒，交接
// 就可以做到不丢帧（新会话的第一个 IDR 一到就切过去，旧会话随后停掉）。
//
// 判据用包计数，但**必须按时刻判**：窗口内计数 > 0 不够，因为一条已经被人顶掉的会话
// 照样能交出几百个包——那是它从起流到那一刻攒在 socket 缓冲区里的欠账，不是活性。
// 所以起 B 之前先把 A 收空（拿到基准线），窗口里只看"最后一个视频包落在哪"，后半段
// 还在发才算并存。两条都有 = 可以并存，交接方案成立；第二条一起来第一条就断流 =
// 设备一条隧道只给一条流，方案作废，那 0.4 秒的顿挫就得换个方式省（比如挪到画面本来
// 就静止的时候做）。顺带对账两条会话的 RTP SSRC 和它们在设备会话表里的状态，用来区分
// "两条独立的流抢一份资源"和"同一条流被复制了一份"。
//
// 用法：two_session_probe [--gap MS] [--verbose]
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hid/Hid.h"
#include "media/StreamSession.h"
#include "remote/Device.h"
#include "rt/RtpHevc.h"

namespace {

using namespace std::chrono_literals;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

/// 从 Annex-B 串里数出 IRAP（IDR/CRA/BLI）。
std::set<int> irap_in(const std::vector<uint8_t> &annexb) {
    std::set<int> found;
    for (std::size_t i = 0; i + 5 < annexb.size(); ++i) {
        if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1) {
            const int type = (annexb[i + 4] >> 1) & 0x3F;
            if (type >= 19 && type <= 21) {
                found.insert(type);
            }
        }
    }
    return found;
}

struct Counters {
    uint64_t video = 0;
    uint64_t sr = 0;
    std::set<int> irap;
    /// 首个/最后一个视频包、首个 IRAP 的**绝对**时刻（0 = 没发生过）。打印时按调用方
    /// 关心的基准换算，因为这条探针里同一个计数要对着两个不同的零点读：A 的活性对着
    /// "B 起流"读，B 的建流延迟也要对着"B 起流"读。
    uint64_t first_video_at = 0;
    uint64_t last_video_at = 0;
    uint64_t first_idr_at = 0;
    /// 设备给这条会话用的 RTP SSRC。两条会话若是各自一个 SSRC，说明它们真是两条流；
    /// 若是同一个，那"第二条"其实只是把同一条流复制了一份到另一个端口。
    uint32_t ssrc = 0;
};

/// 在 `until`（绝对时刻）之前尽量收包，把计数累进 `c`。两个 session 轮流各收一小段，
/// 避免一个收满另一个饿着——这条探针要的就是"两条同时都在发"这个事实。
void drain(scrctl::media::StreamSession &s, Counters &c, std::vector<uint8_t> &buf,
           scrctl::rt::HevcRtpDepacketizer &dp, uint64_t until) {
    std::string err;
    uint16_t peer = 0;
    while (now_ms() < until && s.next_packet(buf, peer, 5, err)) {
        const uint64_t at = now_ms();
        scrctl::rt::PacketInfo info {};
        std::vector<uint8_t> annexb;
        if (scrctl::rt::parse_rtp_header(buf, info) &&
            info.payload_type == s.started().payload_type) {
            ++c.video;
            if (c.first_video_at == 0) {
                c.first_video_at = at;
            }
            c.last_video_at = at;
            if (c.ssrc == 0) {
                c.ssrc = info.ssrc;
            }
            std::ignore = dp.push(buf, annexb, err);
            for (const int t : irap_in(annexb)) {
                if (c.first_idr_at == 0) {
                    c.first_idr_at = at;
                }
                c.irap.insert(t);
            }
        } else if (buf.size() >= 28 && buf[0] == 0x81 && buf[1] == 0xc8) {
            ++c.sr;
        }
    }
}

/// `base` 是这条读数对着的零点（本探针里两条都对着"发出第二条起流请求"那一刻）。
/// 首个包/首个 IDR 就是换一次会话要瞎掉多久，最后一个落在哪就是它到底还活不活。
void report(const char *label, const Counters &c, uint64_t base) {
    std::printf("  %-3s 视频包 %6llu SR %3llu IRAP %s SSRC %08x", label,
                static_cast<unsigned long long>(c.video),
                static_cast<unsigned long long>(c.sr), c.irap.empty() ? "无" : "有", c.ssrc);
    if (c.video == 0) {
        std::printf(" 一个视频包都没收到\n");
        return;
    }
    std::printf(" 首个 +%4llu 首个IDR +%4llu 最后一个 +%4llu（窗口末 +%4llu ms）\n",
                static_cast<unsigned long long>(c.first_video_at - base),
                static_cast<unsigned long long>(c.first_idr_at == 0 ? 0 : c.first_idr_at - base),
                static_cast<unsigned long long>(c.last_video_at - base),
                static_cast<unsigned long long>(now_ms() - base));
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int gap_ms = 1500;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gap" && i + 1 < argc) {
            gap_ms = std::stoi(argv[++i]);
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
    // 全程喂画面变化（交替按音量上/下，音量 HUD 每次按下都浮出淡去，不碰任何 App 的
    // 内容）。这条探针的判据是**视频包计数**，而静止屏幕上设备根本不发视频包——不喂
    // 的话"只有一条在发"和"两条都没在发"会打成同一个读数。
    std::string berr;
    auto buttons = scrctl::hid::Buttons::open(*dev, berr, verbose);
    if (!buttons) {
        std::fprintf(stderr, "开不了按键服务，喂不出画面变化: %s\n", berr.c_str());
        return 1;
    }
    bool up = true;
    uint64_t next_press = 0;
    const uint64_t base = now_ms();
    auto feed = [&] {
        const uint64_t t = now_ms() - base;
        if (t < next_press) {
            return;
        }
        next_press = t + 400;
        std::string perr;
        buttons->press(scrctl::hid::button::kUsagePageConsumer,
                       up ? scrctl::hid::button::kVolumeUp : scrctl::hid::button::kVolumeDown, 60,
                       perr);
        up = !up;
    };

    scrctl::media::StreamSession::Request req;
    std::string e1;
    auto a = scrctl::media::StreamSession::start(*dev, req, e1, verbose);
    if (!a) {
        std::fprintf(stderr, "第一条起流失败: %s\n", e1.c_str());
        return 1;
    }
    scrctl::rt::HevcRtpDepacketizer dpa(a->started().payload_type);
    std::vector<uint8_t> buf;

    std::printf("A 已起（端口 %u）；边喂画面边收 %d ms，再起第二条\n", a->receiver_port(), gap_ms);
    Counters pre;
    const uint64_t gap0 = now_ms();
    uint64_t until_pre = gap0 + static_cast<uint64_t>(gap_ms);
    while (now_ms() < until_pre) {
        feed();
        drain(*a, pre, buf, dpa, std::min<uint64_t>(now_ms() + 25, until_pre));
    }
    std::printf("  起 B 之前：A 在 %d ms 里收到视频包 %llu 个（这是「A 还在发」的基准线）\n", gap_ms,
                static_cast<unsigned long long>(pre.video));

    // 第二条的读数全部对着**发出请求**这一刻读，因为泵将来也是从"决定接续"开始算的：
    // 中间那条 RPC 的时间是真实代价，不能从延迟里抠掉。
    const uint64_t b_start = now_ms();
    std::string e2;
    auto b = scrctl::media::StreamSession::start(*dev, req, e2, verbose);
    if (!b) {
        std::printf("  B 起不来（%llu ms）：%s\n",
                    static_cast<unsigned long long>(now_ms() - b_start), e2.c_str());
        std::printf("结论：设备不接受第二条并发会话，无缝交接方案作废。\n");
        std::string serr;
        a->stop(*dev, serr, verbose);
        return 0;
    }
    std::printf("  B 起流 RPC 用了 %llu ms（端口 %u）；接下来 6 秒两条轮流收\n",
                static_cast<unsigned long long>(now_ms() - b_start), b->receiver_port());

    scrctl::rt::HevcRtpDepacketizer dpb(b->started().payload_type);
    Counters ca, cb;
    const uint64_t until = b_start + 6000;
    while (now_ms() < until) {
        feed();
        drain(*a, ca, buf, dpa, std::min<uint64_t>(now_ms() + 25, until));
        drain(*b, cb, buf, dpb, std::min<uint64_t>(now_ms() + 25, until));
    }
    report("A", ca, b_start);
    report("B", cb, b_start);

    // 两条会话各自的 UUID 在设备的会话表里还在不在。这决定"交接"到底可不可行：
    // 若 A 被顶得连表里都没了，那第二次 startmediastream 是**替换**而不是并存，
    // 换会话必然有一次断流；若 A 还在表里只是不发包，那还有一试的余地（比如它只是
    // 把流改道到新端口，我们可以再把端口换回来）。
    std::string aerr, berr2;
    const auto sa = scrctl::media::StreamSession::probe(*dev, a->started().session_uuid, aerr, verbose);
    const auto sb = scrctl::media::StreamSession::probe(*dev, b->started().session_uuid, berr2, verbose);
    auto state = [](scrctl::media::StreamSession::ServerState s) {
        return s == scrctl::media::StreamSession::ServerState::Alive
                   ? "在"
                   : (s == scrctl::media::StreamSession::ServerState::Ended ? "不在" : "问不到");
    };
    std::printf("  会话表：A %s，B %s\n", state(sa), state(sb));

    // 活性只看"窗口最后 2 秒里还在不在发"。前半段有包只说明它带着存货进来，不说明并存。
    const bool a_still = ca.last_video_at != 0 && ca.last_video_at >= until - 2000;
    const bool b_still = cb.last_video_at != 0 && cb.last_video_at >= until - 2000;
    const bool distinct = ca.ssrc != 0 && cb.ssrc != 0 && ca.ssrc != cb.ssrc;
    std::printf("判读：窗口末 A %s、B %s；SSRC%s。\n", a_still ? "仍在发" : "已停",
                b_still ? "仍在发" : "已停", distinct ? "不同（真是两条流）" : "相同或为 0");
    if (a_still && b_still) {
        std::printf("结论：两条会话可以并存 —— 交接方案成立，先起新的、拿到 IDR 再切过去，"
                    "旧的随后停掉，换会话做到不丢帧。\n");
    } else {
        std::printf("结论：设备一条隧道只给一条流 —— 无缝交接不成立。换一次会话的瞎眼时间"
                    "就是上面 B 那行的「首个 IDR」，只能另想办法藏（比如挪到画面本来就静止"
                    "的时候做）。\n");
    }

    std::string serr;
    a->stop(*dev, serr, verbose);
    b->stop(*dev, serr, verbose);
    return 0;
}
