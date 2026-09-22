#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scrctl::xpc {

/// Apple XPC 的二进制对象图，RemoteXPC 的载荷格式。
///
/// 为什么不引第三方：可用的开源实现（libxpc）是 GPL/LGPL 且面向 Mach 平台，
/// Linux/Windows 上根本没有；而 RemoteXPC 走的是 XPC 的一个子集——所有描述符
/// 都内联在缓冲区里，不存在 out-of-line 的 fd / shared memory 传送，所以完整
/// 的 libxpc 能力我们用不上，反倒是自己写能把边界检查写死。
///
/// 线上格式（全小端）：每个对象 = u32 类型标记 + 该类型的载荷。所有对象的总
/// 长度都是 4 的倍数，字符串与数据后面补零到 4 字节边界，字典的键是「裸的」
/// NUL 结尾字符串同样补到 4 字节边界（注意它和字符串对象不同，**没有**长度前缀）。
enum class Type : uint32_t {
    Null = 0x00001000,
    Bool = 0x00002000,
    Int64 = 0x00003000,
    UInt64 = 0x00004000,
    Double = 0x00005000,
    Date = 0x00007000,
    Data = 0x00008000,
    String = 0x00009000,
    Uuid = 0x0000A000,
    Array = 0x0000E000,
    Dict = 0x0000F000,
    /// 大载荷不进消息本体：字典里放一个 FileTransfer 说明大小，真正的字节由设备
    /// 在另一条 HTTP/2 流上推。截图这类返回值全靠它。
    FileTransfer = 0x0001A000,
};

struct Value;

using Array = std::vector<Value>;

/// 字典条目。先声明、等 Value 定义完再补全，否则「条目里含值、值里含条目」这个
/// 环解不开。vector 从 C++17 起允许不完整元素类型，所以 Value 里只出现
/// vector<Entry> 就够了。
struct Entry;
using Dict = std::vector<Entry>;

struct Value {
    Type type = Type::Null;

    // 标量按实际用到的字段铺开，而不是塞一个 union：类型标记本身就是判别式，
    // 多占几十字节换掉生命周期与对齐的麻烦，值这个价。
    bool boolean = false;
    int64_t int64 = 0;
    uint64_t uint64 = 0;  ///< UInt64 与 Date 共用；Date 是自 epoch 起的纳秒。
    double real = 0;
    std::string string;
    std::vector<uint8_t> data;  ///< Data；Uuid 时长度为 16。
    Array array;
    Dict dict;

    /// 仅 FileTransfer：设备声明的字节数（线上字典里的 "s"）、它的 msg id，
    /// 以及随后从那条推送流上读回来的实际字节。
    ///
    /// data 由通道层填充，不属于线上编码的一部分——所以解出来的 FileTransfer
    /// 在被"取货"之前 data 是空的，直接 encode 会丢内容，这是有意的：文件字节
    /// 从来不在消息里。
    uint64_t file_size = 0;
    uint64_t transfer_id = 0;

    [[nodiscard]] bool is_dict() const { return type == Type::Dict; }
    [[nodiscard]] bool is_array() const { return type == Type::Array; }
    [[nodiscard]] bool is_string() const { return type == Type::String; }

    /// 找不到键返回 nullptr；重复键时取第一个。适合「先判存在再取」的场合。
    [[nodiscard]] const Value *find(std::string_view key) const;
    /// 找不到键返回一个共享的 Null 值引用，因此可以放心链式取值。
    ///
    /// 目录这类外部数据里，某个条目少一个键是常态而不是异常；每处都写判空迟早会
    /// 漏一个（真漏过：解析 RSD 目录时对没有 Entitlement 的条目解引用了空指针，
    /// 表现为随机段错误）。要判存在性请用 find。
    [[nodiscard]] const Value &at(std::string_view key) const;
    [[nodiscard]] std::string as_string_or(std::string_view fallback = {}) const;
    [[nodiscard]] int64_t as_int_or(int64_t fallback = 0) const;
    [[nodiscard]] bool as_bool_or(bool fallback = false) const;
};

/// 条目保持插入顺序：RSD 不关心顺序，但我们要能做字节级往返自检，只有有序容器
/// 才能把「解码 -> 编码 -> 比对」这条路走通。
struct Entry {
    std::string key;
    Value value;
};

// ------------------------------------------------------------- builders ------
Value make_null();
Value make_bool(bool v);
Value make_int64(int64_t v);
Value make_uint64(uint64_t v);
Value make_double(double v);
Value make_date(uint64_t ns_since_epoch);
Value make_string(std::string v);
Value make_data(std::vector<uint8_t> v);
/// 只接受 16 字节；长度不符时解出来的对象编码会失败，而不是悄悄写出畸形数据。
Value make_uuid(std::span<const uint8_t> v);
Value make_file_transfer(uint64_t size, uint64_t transfer_id = 0);
Value make_array();
Value make_dict();

void array_push(Value &arr, Value item);
/// 追加或原地替换；XPC 字典允许重复键，但语义上我们只想要后写覆盖。
void dict_set(Value &dict, std::string key, Value item);

// ------------------------------------------------------------ 对象编解码 ------
/// 把一个对象连同类型标记序列化成一段字节。深度超限（疑似畸形输入）时返回空。
[[nodiscard]] std::vector<uint8_t> encode(const Value &v);

/// 从缓冲区开头解出一个对象。失败返回 nullopt 并给出原因。
[[nodiscard]] std::optional<Value> decode(std::span<const uint8_t> buf, std::string &err);

// ---------------------------------------------------------- 消息（wrapper） ---
inline constexpr uint32_t kWrapperMagic = 0x29B00B92;
inline constexpr uint32_t kPayloadMagic = 0x42133742;
inline constexpr uint32_t kProtocolVersion = 5;

/// wrapper 的 flags 位。名字照 Apple 的 XPC_ACTIVITY_* / 抓包结论。
inline constexpr uint32_t kFlagAlwaysSet = 0x00000001;
inline constexpr uint32_t kFlagPing = 0x00000002;
inline constexpr uint32_t kFlagDataPresent = 0x00000100;
/// 主通道终止帧上带的一位。Apple 没给它名字，抓包里那一帧的 flags 就是
/// 0x0201 = ALWAYS_SET | 这一位。（先前我把它错当成 kFlagIsReply，差了两个
/// 数量级，设备随即把回信通道 RST 掉并报 FRAME_SIZE_ERROR。）
inline constexpr uint32_t kFlagTermChannel = 0x00000200;
inline constexpr uint32_t kFlagWantingReply = 0x00010000;
inline constexpr uint32_t kFlagIsReply = 0x00020000;
inline constexpr uint32_t kFlagFileTxRequest = 0x00100000;
inline constexpr uint32_t kFlagFileTxResponse = 0x00200000;
inline constexpr uint32_t kFlagInitHandshake = 0x00400000;

/// 一条 RemoteXPC 消息。信封布局（全小端）：
///
/// ```text
///  0  u32  wrapper magic 0x29B00B92
///  4  u32  flags
///  8  u64  载荷长度 L        ← 不含下面两个字段，所以整条消息 = 24 + L
/// 16  u64  message_id
/// 24       载荷：u32 magic 0x42133742 + u32 版本 5 + 一个 XPC 对象
/// ```
///
/// 长度记的是「载荷」而非「信封之后的全部」，也就是说 message_id 属于信封。
/// 第一次实现时把长度和消息号的位置记反了，两条都读成对方——因为只有长度恰好
/// 等于消息号时才会碰巧跑通，而那种情况不存在，所以只要拿一条真消息就能发现。
struct Message {
    uint32_t flags = kFlagAlwaysSet;
    uint64_t message_id = 0;
    bool has_body = false;
    Value body;
};

/// body 为空时编成「长度=0、无载荷」（心跳 / 终止帧要的就是这个）。
[[nodiscard]] std::vector<uint8_t> encode_message(uint32_t flags, uint64_t message_id,
                                                  const Value *body);

enum class Status { Ok, NeedMore, Malformed };

/// 从缓冲区开头解出一条完整消息。`consumed` 只在 Ok 时为真，等于这条消息占用的
/// 字节数——可能小于 buf.size()，因为一个 DATA 帧里可能粘了多条消息。
///
/// NeedMore 与 Malformed 必须分开：前者是「继续收」，后者是「链路已经错位，
/// 再收只会更错」。把两者混成一个 optional 的话，半条消息就会被当成协议错误，
/// 连接被白白拆掉。
[[nodiscard]] Status decode_message(std::span<const uint8_t> buf, Message &out, std::size_t &consumed,
                                     std::string &err);

// ----------------------------------------------------------------- 调试 ------
/// 单行可读表示，用于探针打印设备回的服务目录。字符串截断到 120 字节。
[[nodiscard]] std::string describe(const Value &v);

}  // namespace scrctl::xpc
