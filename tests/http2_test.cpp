// HTTP/2 帧层自检。全部离线：帧头是 9 字节的大端结构，任何一个字段写错位，
// 接上真机后表现为「设备一声不吭地把连接拆了」，那种调试成本比现在多写几个
// 断言高得多。
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "http2/Framing.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

using namespace scrctl::http2;

std::string hex(std::span<const uint8_t> b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (uint8_t v : b) {
        s += kHex[v >> 4];
        s += kHex[v & 0xF];
    }
    return s;
}

std::vector<uint8_t> bytes_of(std::initializer_list<int> vals) {
    std::vector<uint8_t> out;
    for (int v : vals) {
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

void test_header_layout() {
    std::printf("\n== 帧头布局 ==\n");
    const auto f = data_frame(1, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("abc"), 3));
    // 长度是 24 位**大端**，这一点和 XPC / usbmux 的小端正好相反，最容易写混。
    check(hex(f) == "000003" "00" "00" "00000001" "616263", "DATA(stream=1, \"abc\")");

    const auto empty = headers_frame(3);
    check(hex(empty) == "000000" "01" "04" "00000003", "空 HEADERS + END_HEADERS，只有 9 字节头");

    // stream id 的最高位是保留位，必须被清掉。
    const auto high = window_update_frame(0x80000001, 1);
    // 帧头 9 字节 = 18 个 hex 字符；流 id 是第 6..9 字节，所以从第 10 个字符起 8 个。
    check(hex(high).substr(10, 8) == "00000001", "stream id 高位被掩掉");

    // 长度字段能表达到 16 MiB − 1，越界就要拒绝而不是回绕。
    const std::vector<uint8_t> big(70000, 0x7f);
    const auto wf = data_frame(1, big);
    check(hex(wf).substr(0, 6) == "011170", "70000 字节 = 0x011170");
    Frame back;
    std::size_t used = 0;
    std::string err;
    check(parse_frame(wf, back, used, err) == Status::Ok && back.payload.size() == 70000,
          "大帧能解回来");

    check(std::string(kClientPreface) == "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" &&
              kClientPrefaceSize == 24,
          "前置签名 24 字节");
}

void test_settings() {
    std::printf("\n== SETTINGS ==\n");
    const auto s = settings_frame({{kSettingMaxConcurrentStreams, 100},
                                   {kSettingInitialWindowSize, 16u << 20}});
    check(hex(s).substr(0, 6) == "00000c", "两个设置 = 12 字节载荷");
    std::vector<std::pair<uint16_t, uint32_t>> values;
    bool ack = true;
    std::string err;
    Frame f;
    std::size_t used = 0;
    check(parse_frame(s, f, used, err) == Status::Ok, "SETTINGS 可解");
    check(parse_settings(f, values, ack, err) && !ack, "非 ACK");
    check(values.size() == 2 && values[0].second == 100 && values[1].second == 16777216,
          "两项设置的值");

    const auto a = settings_ack_frame();
    check(hex(a) == "000000" "04" "01" "00000000", "SETTINGS ACK：空载荷 + 标志位 0x01");
    Frame af;
    check(parse_frame(a, af, used, err) == Status::Ok && parse_settings(af, values, ack, err) && ack,
          "ACK 能认出来");

    // 载荷不是 6 的倍数说明字节流错位了，必须报出来而不是少读一项继续。
    Frame ragged;
    ragged.type = kSettings;
    ragged.payload = bytes_of({0, 1, 0, 0, 0});
    check(!parse_settings(ragged, values, ack, err), "SETTINGS 载荷长度奇数被拒");

    const std::pair<uint16_t, uint32_t> odd = {0x5, 0x4000};
    const auto one = settings_frame({odd});
    check(hex(one).substr(18) == "000500004000", "单个设置项的大端编码");
}

void test_flow_control_frames() {
    std::printf("\n== WINDOW_UPDATE / GOAWAY / RST ==\n");
    const auto w = window_update_frame(0, 16u << 20);
    uint32_t inc = 0;
    std::string err;
    Frame f;
    std::size_t used = 0;
    check(parse_frame(w, f, used, err) == Status::Ok && f.type == kWindowUpdate &&
              parse_window_update(f, inc, err) && inc == 16777216,
          "全连接窗口 +16 MiB");

    // 增量也受保留位约束，读回来不能带上符号位。
    Frame masked;
    masked.payload = bytes_of({0x80, 0x00, 0x00, 0x01});
    check(parse_window_update(masked, inc, err) && inc == 1, "WINDOW_UPDATE 高位被掩掉");

    std::string debug = "Invalid or missing remote device connection version flags\x01\n";
    std::vector<uint8_t> body;
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    body.push_back(3);
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    body.push_back(1);
    body.insert(body.end(), debug.begin(), debug.end());
    const auto g = serialize(kGoAway, 0, 0, body);
    Frame gf;
    GoAway goaway;
    check(parse_frame(g, gf, used, err) == Status::Ok && parse_goaway(gf, goaway, err),
          "GOAWAY 可解: " + err);
    check(goaway.last_stream_id == 3, "last_stream_id");
    check(goaway.error_code == kProtocolError, "错误码");
    check(goaway.debug_data == "Invalid or missing remote device connection version flags",
          "调试串里的控制字符被滤掉: " + goaway.debug_data);
    check(describe(gf).find("PROTOCOL_ERROR") != std::string::npos &&
              describe(gf).find("last=3") != std::string::npos,
          "describe 把错误码和人话一起打出来: " + describe(gf));

    Frame short_goaway;
    short_goaway.payload = bytes_of({0, 0, 0});
    check(!parse_goaway(short_goaway, goaway, err), "GOAWAY 不足 8 字节被拒");

    const auto r = serialize(kRstStream, 0, 1, bytes_of({0, 0, 0, 3}));
    Frame rf;
    uint32_t code = 0;
    check(parse_frame(r, rf, used, err) == Status::Ok && parse_rst_stream(rf, code, err) &&
              code == kFlowControlError,
          "RST_STREAM 错误码");
    check(describe(rf).find("FLOW_CONTROL_ERROR") != std::string::npos, "RST 的 describe");
}

void test_data_padding() {
    std::printf("\n== DATA 的填充与未定义标志 ==\n");
    std::string err;
    std::span<const uint8_t> out;

    // Pad Length 在最前一个字节，填充字节在最后——顺序读反了就会把长度当数据。
    auto padded = Frame{};
    padded.flags = kFlagPadded;
    padded.payload = bytes_of({0x02, 'a', 'b', 0xff, 0xff});
    check(data_payload(padded, out, err) && std::string(out.begin(), out.end()) == "ab",
          "首字节声明 2，尾部 2 字节填充被剥掉");

    // 只有 Pad Length、整帧都是填充：合法，载荷为空。
    auto only_padding = Frame{};
    only_padding.flags = kFlagPadded;
    only_padding.payload = bytes_of({0x01, 0x00});
    check(data_payload(only_padding, out, err) && out.empty(), "整帧都是填充是合法的");

    // 纯 DATA 载荷不能被尾巴上的 0 误伤（padding=0 时不能再减一个字节）。
    auto zero_pad = Frame{};
    zero_pad.flags = kFlagPadded;
    zero_pad.payload = bytes_of({0x00, 'a', 'b', 'c'});
    check(data_payload(zero_pad, out, err) && std::string(out.begin(), out.end()) == "abc",
          "Pad Length=0 时载荷完整保留");

    Frame overlong;
    overlong.flags = kFlagPadded;
    overlong.payload = bytes_of({0x05, 0x00});
    check(!data_payload(overlong, out, err), "Pad Length 超过可用载荷被拒: " + err);

    Frame empty_padded;
    empty_padded.flags = kFlagPadded;
    check(!data_payload(empty_padded, out, err), "PADDED 却没有 Pad Length 字节被拒");

    // 0x20 在 DATA 上没有定义，按 §4.1 必须忽略。把它当 5 字节优先级前缀剥掉
    // 是最糟的错法：载荷会凭空少 5 字节，而那 5 字节其实是 XPC 消息的开头。
    auto undefined = Frame{};
    undefined.flags = kFlagPriority | kFlagEndStream;
    undefined.payload = bytes_of({0xde, 0xad, 0xbe, 0xef, 0x01, 0x02});
    check(data_payload(undefined, out, err) && out.size() == 6,
          "未定义标志被忽略，载荷一个字节都不少");
}

void test_stream_boundaries() {
    std::printf("\n== 粘包与半包 ==\n");
    std::vector<uint8_t> buf;
    const auto a = settings_frame({{kSettingInitialWindowSize, 7}});
    const auto b = ping_frame(0x0102030405060708, false);
    const auto c = headers_frame(5);
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), b.begin(), b.end());
    buf.insert(buf.end(), c.begin(), c.end());

    std::string err;
    std::size_t offset = 0;
    std::vector<std::string> types;
    while (offset < buf.size()) {
        Frame f;
        std::size_t used = 0;
        const auto st = parse_frame({buf.data() + offset, buf.size() - offset}, f, used, err);
        if (st != Status::Ok) {
            check(false, "逐帧切开时应一路 Ok: " + err);
            return;
        }
        types.push_back(frame_type_name(f.type));
        offset += used;
    }
    check(types.size() == 3 && types[0] == "SETTINGS" && types[1] == "PING" &&
              types[2] == "HEADERS",
          "一包三帧，按声明长度依次切开");

    const auto p = ping_frame(0x0102030405060708, false);
    check(hex(p).substr(18) == "0102030405060708", "PING 的不透明数据是大端 8 字节");

    for (std::size_t cut = 0; cut < a.size(); ++cut) {
        Frame f;
        std::size_t used = 0;
        if (parse_frame({a.data(), cut}, f, used, err) != Status::NeedMore) {
            check(false, "截断到 " + std::to_string(cut) + " 字节应报 NeedMore");
            return;
        }
    }
    check(true, "每个截断长度都是 NeedMore，而不是 Malformed");

    Frame f;
    std::size_t used = 0;
    std::vector<uint8_t> bogus = bytes_of({0xff, 0xff, 0xff, kData, 0, 0, 0, 0, 1});
    check(parse_frame(bogus, f, used, err) == Status::Malformed,
          "帧长 16 MiB−1 超出实现上限: " + err);

    check(parse_frame(bytes_of({0, 0}), f, used, err) == Status::NeedMore, "2 字节连帧头都不够");

    // 未知帧类型：RFC 要求能安全忽略，所以这里必须照常 Ok，让上层按类型丢弃。
    const auto unknown = serialize(0x9a, 0, 0, bytes_of({1, 2, 3}));
    check(parse_frame(unknown, f, used, err) == Status::Ok && f.type == 0x9a &&
              f.payload.size() == 3,
          "未知帧类型照样解出，交上层忽略");
}

}  // namespace

int main() {
    test_header_layout();
    test_settings();
    test_flow_control_frames();
    test_data_padding();
    test_stream_boundaries();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
