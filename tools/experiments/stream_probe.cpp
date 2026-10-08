// 探针：起真机视频流，把 RTP 包原样收下来。
//
// 这一步要回答的问题都只能靠真机答：
//   1. 设备能不能把 UDP 打穿我们自己的用户态栈（隧道内反向推）；
//   2. 协商到的 PT 是多少、视频载荷到底是什么形态——是不是一帧一包、一包里几个
//      NAL、分片怎么标边界。
// 所以先把包原样落盘再离线看，不急着解释。事后证明这是对的决定：载荷头之后的
// 8 字节一度被当成"苹果私有子头"，而真正的形状要从原始字节上量出来才看得见。
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "media/StreamSession.h"
#include "remote/Device.h"

namespace {

void hexdump(const std::vector<uint8_t> &b, std::size_t n) {
    for (std::size_t i = 0; i < n && i < b.size(); ++i) {
        std::printf("%02x", b[i]);
        if (i % 8 == 7) {
            std::printf(" ");
        }
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool verbose = false;
    bool try_stop = false;
    std::string out = "/tmp/rtp.bin";
    int seconds = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--stop") {
            try_stop = true;
        } else if (a == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == "-t" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        }
    }

    std::string err;
    auto dev = scrctl::remote::Device::establish({}, err, verbose);
    if (!dev) {
        std::fprintf(stderr, "建立会话失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("会话就绪：%s / iOS %s\n", dev->property("ProductType").c_str(),
                dev->property("OSVersion").c_str());

    scrctl::media::StreamSession::Request request;
    auto session = scrctl::media::StreamSession::start(*dev, request, err, verbose);
    if (!session) {
        std::fprintf(stderr, "起流失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("已起流：本机收流端口=%u 设备发送端口=%u\n", session->receiver_port(),
                session->started().sender_port);
    std::printf("answer: %s\n", scrctl::xpc::describe(session->started().answer).substr(0, 900).c_str());

    FILE *f = std::fopen(out.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "写 %s 失败\n", out.c_str());
        return 1;
    }
    std::vector<uint8_t> packet;
    std::size_t count = 0, bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                            start)
               .count() < seconds) {
        if (!session->next_packet(packet, 1000, err)) {
            if (count == 0) {
                std::fprintf(stderr, "一个包都没收到: %s\n", err.c_str());
            }
            break;
        }
        const uint32_t len = static_cast<uint32_t>(packet.size());
        std::fwrite(&len, 4, 1, f);
        std::fwrite(packet.data(), 1, packet.size(), f);
        if (count < 3) {
            std::printf("包 %zu: %u 字节, 前 32 字节: ", count, len);
            hexdump(packet, 32);
        }
        ++count;
        bytes += packet.size();
    }
    std::fclose(f);
    const double secs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count() /
                        1000.0;
    std::printf("共 %zu 个包 / %zu 字节，%.2f 秒 -> %.1f 包/秒, %.2f Mbps\n", count, bytes, secs,
                count / secs, bytes * 8.0 / secs / 1e6);
    std::printf("已存 %s\n", out.c_str());

    if (try_stop) {
        // stopmediastream 的入参形状没记录。设备的错误信息会点名缺哪个键
        // （"Expected to decode UUID but found a OS_xpc_string" 之类），
        // 所以一次试几种形状、把每种的回话原样打出来，比照着别的实现猜快。
        auto typed = [](std::string_view kind, scrctl::xpc::Value v) {
            scrctl::xpc::Value w = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(w, std::string(kind), std::move(v));
            return w;
        };
        const auto uuid = scrctl::xpc::make_uuid(std::span<const uint8_t>(session->started().session_uuid));
        struct Candidate {
            const char *label;
            std::string action;
            scrctl::xpc::Value input;
        };
        std::vector<Candidate> cands;
        // 第一轮设备对四种形状都回 "Expected to find key stopAll."，所以这一轮
        // 全带上 stopAll，只变它的类型和伴生键。
        {
            auto in = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(in, "stopAll", scrctl::xpc::make_bool(true));
            cands.push_back({"stopAll=true", "com.apple.coredevice.action.mediastreamstop",
                             std::move(in)});
        }
        {
            auto in = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(in, "stopAll", scrctl::xpc::make_bool(true));
            scrctl::xpc::dict_set(in, "type", scrctl::xpc::make_string("video"));
            cands.push_back({"stopAll=true + type", "com.apple.coredevice.action.mediastreamstop",
                             std::move(in)});
        }
        {
            auto in = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(in, "stopAll", scrctl::xpc::make_uint64(1));
            cands.push_back({"stopAll=uint64 1", "com.apple.coredevice.action.mediastreamstop",
                             std::move(in)});
        }
        {
            auto in = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(in, "stopAll", scrctl::xpc::make_bool(false));
            auto opts = scrctl::xpc::make_dict();
            scrctl::xpc::dict_set(opts, "avcMediaStreamOptionClientSessionID",
                                  typed("uuid", uuid));
            scrctl::xpc::dict_set(in, "options", std::move(opts));
            scrctl::xpc::dict_set(in, "type", scrctl::xpc::make_string("video"));
            cands.push_back({"stopAll=false + ClientSessionID",
                             "com.apple.coredevice.action.mediastreamstop", std::move(in)});
        }
        for (auto &c : cands) {
            scrctl::xpc::Value out_value;
            std::string serr;
            const char *action = c.action.empty()
                                     ? "com.apple.coredevice.action.mediastreamstop"
                                     : c.action.c_str();
            const bool ok = dev->feature("com.apple.coredevice.displayservice",
                                            "com.apple.coredevice.feature.stopmediastream", action,
                                            c.input, out_value, serr, verbose, 8000);
            std::printf("\nstop 形状[%s] -> %s\n", c.label,
                        ok ? ("成功: " + scrctl::xpc::describe(out_value)).c_str() : serr.c_str());
        }
    }
    return count > 0 ? 0 : 1;
}
