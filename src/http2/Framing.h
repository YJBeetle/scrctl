#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// HTTP/2 帧层，只到「帧」为止，不含 HPACK。
///
/// 为什么不实现 HPACK：RemoteXPC 的 HEADERS 帧是**空的**——建流只发一个带
/// END_HEADERS 的 9 字节帧头，路由信息全在 stream id 里（1 是主通道，3 是
/// 回信通道），XPC 载荷走 DATA 帧。设备回给我们的 HEADERS 我们不需要看懂，
/// 跳过其载荷即可。少一整套表 + 哈夫曼码表，是这条链路上最值得的一次减法。
///
/// 为什么不用 nghttp2：它是 LGPL，且整个「会话/优先级/流控/HPACK」状态机
/// 对我们是负资产——这里只用到 6 种帧类型。RFC 7540 的帧头是 9 字节，自己
/// 收发反而能把边界检查写死。
namespace scrctl::http2 {

inline constexpr std::size_t kFrameHeaderSize = 9;
/// 实现侧允许的单帧上限。RFC 允许 16 MiB，但那是外部输入能声明的数字，
/// 我们不预备这么大的缓冲；超了这个值就按协议错误处理。
inline constexpr std::size_t kMaxFrameSize = 1u << 22;
/// 双方未通过 SETTINGS 协商前的默认值（RFC 7540 §6.5.2 / §6.9.2）。
inline constexpr uint32_t kDefaultInitialWindowSize = 65535;
inline constexpr std::size_t kDefaultMaxFrameSize = 16384;

/// 客户端前置签名：连接上最先写的 24 字节，不是帧。
constexpr const char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t kClientPrefaceSize = 24;

enum Type : uint8_t {
    kData = 0x0,
    kHeaders = 0x1,
    kPriority = 0x2,
    kRstStream = 0x3,
    kSettings = 0x4,
    kPushPromise = 0x5,
    kPing = 0x6,
    kGoAway = 0x7,
    kWindowUpdate = 0x8,
    kContinuation = 0x9,
};

/// 帧标志。统一带 kFlag 前缀：标志位和帧类型共享同一批小整数，不加前缀就会
/// 撞名（kPriority 同时是帧类型 0x2 和标志位 0x20）。另外 kFlagAck 与
/// kFlagEndStream 同值但互不冲突——前者只出现在 SETTINGS/PING 上，后者只出现
/// 在 DATA/HEADERS 上。
enum Flag : uint8_t {
    kFlagEndStream = 0x1,
    kFlagAck = 0x1,
    kFlagEndHeaders = 0x4,
    kFlagPadded = 0x8,
    kFlagPriority = 0x20,
};

enum Setting : uint16_t {
    kSettingHeaderTableSize = 0x1,
    kSettingEnablePush = 0x2,
    kSettingMaxConcurrentStreams = 0x3,
    kSettingInitialWindowSize = 0x4,
    kSettingMaxFrameSize = 0x5,
};

/// RFC 7540 §7 的错误码，出现在 GOAWAY / RST_STREAM 里。
enum ErrorCode : uint32_t {
    kNoError = 0x0,
    kProtocolError = 0x1,
    kInternalError = 0x2,
    kFlowControlError = 0x3,
    kFrameSizeError = 0x5,
    kCompressionError = 0x9,
};

const char *frame_type_name(uint8_t type);
const char *error_code_name(uint32_t code);

/// 一个已经收全的帧。载荷按原样保留，由调用方按 type 再解。
struct Frame {
    uint8_t type = kData;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

enum class Status { Ok, NeedMore, Malformed };

/// 序列化一个帧头 + 载荷。
[[nodiscard]] std::vector<uint8_t> serialize(uint8_t type, uint8_t flags, uint32_t stream_id,
                                             std::span<const uint8_t> payload = {});

// ------------------------------------------------------------ 便捷构造 ------
[[nodiscard]] std::vector<uint8_t> settings_frame(
    const std::vector<std::pair<uint16_t, uint32_t>> &settings);
[[nodiscard]] std::vector<uint8_t> settings_ack_frame();
[[nodiscard]] std::vector<uint8_t> window_update_frame(uint32_t stream_id, uint32_t increment);
[[nodiscard]] std::vector<uint8_t> headers_frame(uint32_t stream_id);
[[nodiscard]] std::vector<uint8_t> data_frame(uint32_t stream_id, std::span<const uint8_t> payload,
                                              uint8_t extra_flags = 0);
[[nodiscard]] std::vector<uint8_t> ping_frame(uint64_t opaque_data, bool ack);

// ---------------------------------------------------------------- 解析 ------
/// 从缓冲区开头取出一整帧。`consumed` 仅在 Ok 时为帧长（可能小于 buf.size()，
/// 一包里可能粘了多帧）。
///
/// NeedMore 与 Malformed 分开返回：前者要继续收，后者说明字节流已经错位，
/// 再收只会更错——把两者混在一起，半帧会被当成协议错误而白白拆掉连接。
Status parse_frame(std::span<const uint8_t> buf, Frame &out, std::size_t &consumed,
                   std::string &err);

/// DATA 载荷：按 PADDED 剥掉首字节的 Pad Length 与尾部填充。其余标志未定义，
/// 按规范忽略（不是「未知就跳过」的敷衍——把未定义标志当可选字段读，会吃掉真载荷）。
bool data_payload(const Frame &f, std::span<const uint8_t> &out, std::string &err);

/// SETTINGS 载荷 -> (标识符, 值) 列表；`ack` 回带 ACK 标志。
bool parse_settings(const Frame &f, std::vector<std::pair<uint16_t, uint32_t>> &out, bool &ack,
                    std::string &err);

/// WINDOW_UPDATE 的增量。stream 0 是全连接，其余是单流。
bool parse_window_update(const Frame &f, uint32_t &increment, std::string &err);

struct GoAway {
    uint32_t last_stream_id = 0;
    uint32_t error_code = 0;
    std::string debug_data;  ///< 设备常在这里写人话，比如 "Invalid or missing remote ..."
};
bool parse_goaway(const Frame &f, GoAway &out, std::string &err);

bool parse_rst_stream(const Frame &f, uint32_t &error_code, std::string &err);

[[nodiscard]] std::string describe(const Frame &f);

}  // namespace scrctl::http2
