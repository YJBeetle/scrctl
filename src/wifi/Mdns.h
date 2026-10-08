#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scrctl::wifi::mdns {

struct Endpoint {
    /// IPv6 link-local 地址带 %接口索引，可直接交给本机连接接口。
    std::string address;
    uint16_t port = 0;
    uint32_t interface_index = 0;
    std::string interface_name;
    bool operator==(const Endpoint &) const = default;
};

/// 一条 _remotepairing._tcp.local. 广播。广播标识不是 USB UDID，TXT 也不是认证结果。
/// 同一个实例在不同接口上的记录分别关联，最后保留所有地址和真实 SRV 端口。
struct Advertisement {
    std::string instance;
    std::string identifier;
    std::string auth_tag;  ///< TXT authTag 原始字节，可能不是可打印字符串
    std::string target;
    std::string name;  ///< TXT name/displayName 有值时提供，否则为空
    std::map<std::string, std::string> txt;  ///< 键转 ASCII 小写，值保持原始字节
    std::vector<Endpoint> endpoints;
};

struct BrowseOptions {
    std::chrono::milliseconds timeout{3000};
    /// 留空表示不取消。回调应快速返回且不抛异常；共享状态由调用方同步。
    /// 可捕获 std::stop_token 或原子标志，不要求系统 libc++ 已公开 stop_token。
    std::function<bool()> should_cancel;
};

struct BrowseResult {
    std::vector<Advertisement> advertisements;
    bool cancelled = false;
    bool available = false;  ///< 至少一个接口成功发送查询；空列表不代表扫描失败
    std::vector<std::string> warnings;
};

/// 使用各个已启用的组播网卡做有期限的查询，不配对、不连接设备服务。
/// 零超时直接返回空快照，不枚举接口或发包。取消保留已收到的快照并置 cancelled，
/// 收包等待最多 50 ms；接口或网络错误放入 warnings，不写终端。
/// 超时范围是 0..60 秒；超过范围返回 warning，不进行网络操作。
BrowseResult browse(const BrowseOptions &options = {});

namespace detail {

/// 扫描器的报文聚合器；单独开放这条内部边界，方便离线验证压缩名称和跨报文关联。
/// 不缓存到下一次扫描。now 参数只用于本次扫描的 TTL 判断。
class RecordCache {
  public:
    using Clock = std::chrono::steady_clock;
    RecordCache();
    ~RecordCache();
    RecordCache(const RecordCache &) = delete;
    RecordCache &operator=(const RecordCache &) = delete;
    /// interface_index 是同一网卡的记录关联索引及 IPv4 索引；Windows 的 IPv6
    /// 索引另传 ipv6_scope_index，用于 AAAA 候选和 link-local scope。省略时沿用
    /// interface_index；显式为 0 表示该网卡没有可用的 IPv6 索引。
    bool add_packet(uint32_t interface_index, std::string_view interface_name,
                    std::span<const uint8_t> packet, Clock::time_point now = Clock::now(),
                    std::optional<uint32_t> ipv6_scope_index = std::nullopt);
    std::vector<Advertisement> snapshot(Clock::time_point now = Clock::now()) const;
    /// 尚缺的 SRV/TXT 或主机 A/AAAA 查询，按接口隔离。
    std::vector<std::pair<uint16_t, std::string>> queries(uint32_t interface_index,
                                                        Clock::time_point now = Clock::now()) const;
    const std::vector<std::string> &warnings() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detail
} // namespace scrctl::wifi::mdns
