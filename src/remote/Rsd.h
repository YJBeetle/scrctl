#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/Stack.h"
#include "net/TcpStream.h"
#include "remote/RemoteXpc.h"
#include "xpc/XpcValue.h"

namespace scrctl::remote {

/// RSD 目录里一个服务的条目。
///
/// 注意 `Port` 在线上是个**字符串**而不是整数——设备就是这么发的，按整数读会
/// 拿到 0，然后连接被拒，现场看起来像"权限不够"。
struct ServiceInfo {
    std::string name;
    uint16_t port = 0;
    bool uses_remote_xpc = false;
    bool encrypt_socket_data = false;
    std::string entitlement;
    std::vector<std::string> features;
};

/// 一次 feature 调用的三种结局。
///
/// 必须把"设备答了、但说不同意"和"根本没答上"分开：前者重试多少次都是同一个错
/// （参数就是不对），后者往往换一条连接再发一次就成（实测约 15% 的服务连接会撞上
/// 超时/帧错位/对端关闭）。混成一个 bool，要么该重试的不重试，要么不该重试的白等。
enum class CallResult { Ok, DeviceError, TransportError };

/// 一条已打开的服务连接。XPC 类服务在 TCP 之上再走一遍 HTTP/2 + RemoteXPC
/// 握手；lockdown shim 那批（UsesRemoteXPC=false）只是裸协议，留成裸 socket。
class ServiceConnection {
public:
    ServiceConnection() = default;

    static std::unique_ptr<ServiceConnection> open(net::Stack &stack, const ServiceInfo &service,
                                                   std::string &err, bool verbose = false);

    /// 调一个 CoreDevice feature。`input` 放进 CoreDevice.input，回信取
    /// CoreDevice.output；设备侧失败时把 CoreDevice.error 里的话带在 err 里。
    CallResult invoke(std::string_view feature_identifier, std::string_view action_identifier,
                      const xpc::Value &input, xpc::Value &output, int timeout_ms,
                      std::string &err);

    /// 直接一发一收，供非 CoreDevice 封装的服务用。
    bool call(const xpc::Value &request, xpc::Value &reply, int timeout_ms, std::string &err);

    /// 只发不收。HID 报告这类"投出去就完"的请求必须走这条路：设备对它们不安
    /// 回信，用 call() 就是每个点等一次超时，注入延迟立刻变成秒级。
    bool send_only(const xpc::Value &request, std::string &err);

    /// 原始收信：不套 CoreDevice 那层 feature 信封，回信原样交出。
    ///
    /// 为什么不能用 call()：RemotePairing 的控制通道不是一发一收的形状——设备可能
    /// 先回一条 `awaitingUserConsent` 再回数据（两条回信对一次发送），也可能干脆
    /// 不回（iOS 27 的 verify 探针就是这样，docs §25.6）。发与等必须能分开调，
    /// 发那一半用上面的 `send_only`（参考实现在这条通道上也不带 WANTING_REPLY）。
    /// Timeout 与 Broken 分开交出来，理由同 `Channel::wait`。
    Channel::Wait wait_message(xpc::Value &out, int timeout_ms, std::string &err);

    /// 流式 feature：一次请求、**多条**回信，直到设备发 finishStreaming。
    ///
    /// 为什么要有它：`listapps` 把全部 App 一次性装进一个回信，在这台设备上是几 MB，
    /// 而大回复正是我们传不稳的那一类（docs §15）；实测它还会因为 include* 为 true
    /// 直接 60 秒不回话。流式的那条（`streamapplist` / `streamprocesslist`）把同样的
    /// 内容切成一批一批的小回信，绕开尺寸问题。
    ///
    /// 协议形状（设备侧是 CoreDeviceUtilities 的 StreamingAction.swift）：请求把参数
    /// 裹在 `CoreDevice.input.actualInput` 下，并给一个
    /// `streamProxy.sideChannel` = 客户端自己生成的 UUID；回信一串，每条带
    /// `CoreDevice.XPCMessageKey.sideChannelStatus`，值是枚举
    /// `pushing:{elements:[...]}` / `finishStreaming:{}` / `receivedError:...`。
    ///
    /// on_element 对每个元素调一次；返回 false 表示"够了，别推了"（比如只找一个
    /// bundle id，命中就可以收工）。
    CallResult stream(std::string_view feature_identifier, std::string_view action_identifier,
                      const xpc::Value &input,
                      const std::function<bool(const xpc::Value &)> &on_element, int timeout_ms,
                      std::string &err);

    /// 流式订阅的分步版本：`subscribe` 订上，`next_batch` 一批一批取。
    ///
    /// 与 `stream()` 的区别只有一件事——**能不能中途干净地停**。`stream()` 是阻塞
    /// 到底的循环，它的 timeout 是"单条消息"的等待上限而不是整轮的预算，所以拿它做
    /// 常驻订阅的话，收手最快也要等一个 timeout；而"跟着转屏改画面"这类订阅要挂很久、
    /// 又必须随时能退（Ctrl-C 的响应时间就是它）。分步之后每次只等一小会儿，
    /// 之间回到调用方的循环顶部看一眼该不该停。
    bool subscribe(std::string_view feature_identifier, std::string_view action_identifier,
                   const xpc::Value &input, std::string &err);

    /// 一次取批的结局。Idle 是**常态**（设备只在状态真的变了才推，实测订阅后 21 秒
    /// 可以一条都不发）；只有 DeviceError/Broken 才意味着这条订阅作废、要重连。
    enum class StreamEvent { Batch, Finished, Idle, DeviceError, Broken };

    /// 取下一批 element（一条 `pushing` 可以带多个；空批次也会以 Batch 返回，
    /// 调用方继续等就是）。`elements` 每次先被清空。
    StreamEvent next_batch(std::vector<xpc::Value> &elements, int timeout_ms, std::string &err);

    [[nodiscard]] net::TcpStream &tcp() { return *tcp_; }
    [[nodiscard]] bool is_xpc() const { return channel_ != nullptr; }

    /// 空转期间替这条连接读一眼（处理设备的 PING / WINDOW_UPDATE）。
    /// false 只表示链路真断了，读超时不算。
    bool service(int timeout_ms, std::string &err);

private:
    std::unique_ptr<net::TcpStream> tcp_;
    std::unique_ptr<Channel> channel_;
    /// 当前那次订阅的 feature 名。只为把错误说全：`next_batch` 拿到的是一条
    /// 没有请求号的回信（`take_message` 不看消息 id），出了错只有"是哪条订阅在跑"
    /// 这个线索能把话说明白。
    std::string subscribed_feature_;
};

/// 一条隧道 + 它的 RSD 服务目录。
class Rsd {
public:
    /// 在隧道的 RSD 端口上建控制通道并读目录。
    static std::optional<Rsd> open(net::Stack &stack, transport::PacketTunnel &tunnel,
                                   const PeerIdentity &identity, std::string &err,
                                   bool verbose = false);

    /// 目录里没有该项时返回 nullopt。
    [[nodiscard]] std::optional<ServiceInfo> service(std::string_view name) const;
    [[nodiscard]] bool has_service(std::string_view name) const;
    /// 某服务是否声明了这个 feature。调用前查一遍，比让设备回一个语义模糊的
    /// 错误信息好——DDI 版本不同 feature 集合就不同。
    [[nodiscard]] bool supports(std::string_view service_name, std::string_view feature) const;
    [[nodiscard]] const std::vector<ServiceInfo> &services() const { return services_; }

    /// "目录里没有这个服务"那句报错的完整措辞：**把目录里实际看到的一起打出来**。
    ///
    /// 理由是一次外部反馈：那台 iPad mini(iOS 18) 走到"设备目录里没有 displayservice"
    /// 就停了，而我们只报缺什么、不报有什么，远程就分不出"DDI 根本没挂"和"这版系统的
    /// DeviceKit 没实现这套服务"——两种都要改代码，改法完全不同。设备目录是它自己发的
    /// 权威清单，所以这一段才是决定性数据。纯函数（输入是个列表），因此离线就能判。
    static std::string missing_service_message(std::string_view name,
                                               const std::vector<ServiceInfo> &seen);

    /// 设备回 "…requires iOS 27.0 or later on this device"（code 9021）时的补白。
    /// 光转述设备原话，用户会照着去查 DDI 与配对——而这两样都是好的：门槛在设备侧
    /// 的系统版本上（iPadOS 18.7.8 实测，docs §23）。纯函数，离线可判。
    static std::string remote_control_version_hint(std::string_view device_detail);

    /// 目录里这一项的**原始** XPC 字典。解析后的结构体只覆盖了我预料得到的键，
    /// 排查"为什么设备不回话"时用得着没被解读的那些字段。
    [[nodiscard]] const xpc::Value *service_entry(std::string_view name) const;

    std::unique_ptr<ServiceConnection> connect_service(std::string_view name, std::string &err,
                                                      bool verbose = false);

    /// 设备自报的 Properties（型号、OS 版本、UDID 等）。敏感字段调用方自己决定怎么打。
    [[nodiscard]] const xpc::Value *properties() const;

    [[nodiscard]] net::Stack &stack() { return *stack_; }
    [[nodiscard]] const PeerIdentity &identity() const { return identity_; }

    /// 公开的构造函数只为 std::optional::emplace 能建它（optional 的内部实现
    /// 在本类作用域之外，私有构造它调不动）。造出来的实例还没握手，别直接用，
    /// 要可用的目录请走 open()。
    Rsd(net::Stack &stack, PeerIdentity identity)
        : stack_(&stack), identity_(std::move(identity)) {}

private:

    /// 用指针而不是引用：引用成员会让拷贝/移动赋值全变成 deleted，于是
    /// std::optional<Rsd> 也没法赋值，Device 想按 `rsd_ = Rsd::open(...)` 装配
    /// 就得改道路。栈本身的生命周期由 Device 保证，指针在这儿是安全的。
    net::Stack *stack_ = nullptr;
    PeerIdentity identity_;
    /// Channel 借引用用 TcpStream，两个所有权都得在这儿，且 tcp_ 必须声明在
    /// control_ 之后——析构是反序的，得让借方先走。
    std::unique_ptr<net::TcpStream> tcp_;
    std::unique_ptr<Channel> control_;
    std::vector<ServiceInfo> services_;
};

/// 造一个随机的 UUID v4 文本。CoreDevice 请求里的 invocation / device 标识用它，
/// 这两个是每次调用都不一样的追踪号，没有稳定性要求（区别于 peer UUID，
/// 那个必须跨进程稳定，见 PeerIdentity 的注释）。
[[nodiscard]] std::string random_uuid_text();

/// 组装 CoreDevice 那条统一的请求外壳。
[[nodiscard]] xpc::Value core_device_request(std::string_view feature_identifier,
                                             std::string_view action_identifier,
                                             const xpc::Value &input);

}  // namespace scrctl::remote
