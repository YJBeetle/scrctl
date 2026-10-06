# lwIP 隧道适配验证

## 当前结论（2026-10-07）

lwIP 2.2.1 可以通过自定义 netif 对接现有 IPv6 包隧道。独立探针的 TCP 重传、
乱序重组、多连接和 UDP 往返测试通过，USB 真机的 RSD 目录与大截图响应、Wi-Fi 的目录与截图也已通过。
这些结果支持继续做生产适配，但本轮没有替换生产 Stack / TcpStream / UdpSocket。

RemoteXPC 现在只依赖内部 ByteStream 的 send / recv 接口。探针复用同一份 HTTP/2、
XPC、身份申报、文件处理和请求代码；没有另写一份 RemoteXPC 来证明 TCP 可用。
生产仍创建原 TcpStream，未引入 lwIP 依赖。

## 验证方式与结果

| 项目 | 判据 | 结果 |
| --- | --- | --- |
| IPv6 包接口 | netif 的 output_ip6 输出完整 IPv6 包，input 接收完整 IPv6 包 | 通过 |
| SYN 重传 | 丢掉首个 SYN，连接仍建立，服务端只接受一次 | 通过 |
| TCP 数据重传 | 丢掉一个数据段，200,000 字节全部收到且 ACK 完整 | 通过 |
| RTO 定时重传 | 另一条连接只发一个段并丢弃，没有后续数据生成重复 ACK，定时器恢复传输 | 通过 |
| 乱序接收 | 服务端两个数据段交换顺序，完整字节序列与原始内容一致 | 通过 |
| 发送缓冲压力 | 一次发送的数据超过 65,535 字节发送缓冲，继续驱动收包/ACK 后全部排入 | 通过 |
| 多连接 | 两条同时建立的连接分别接收自己的数据 | 通过 |
| 关闭 | 丢掉首个 FIN，定时器重传后对端收到 EOF | 通过 |
| 超时与错误 | 空读为超时，RST 为错误；二者传递不同结果 | 通过 |
| UDP | 非零和高位字节的奇数长度载荷经 IPv6 往返，接收端校验并逐字节比较 | 通过 |
| USB RSD | lwIP TCP 接现有 RemoteXPC，身份申报后读到 85 项服务，隧道 MTU 16,000 | 通过 |
| USB 截图 | RSD 保持打开，同时用第二条连接取得 PNG 并校验文件签名 | 通过，分别收到 2,050,955 和 1,074,668 字节 |
| Wi-Fi RSD / 截图 | 经局域网地址与 en0 路径，现有远程配对验证、TLS-PSK 隧道、85 项目录和第二连接截图 | 通过；先后取得 76,943 和 274,780 字节图片 |
| 内存访问 | AddressSanitizer 构建运行离线探针，覆盖错误回调和关闭后定时器 | 通过，未报告内存访问错误 |
| 生产回归 | 原 TCP 后端的 scrctl 经 USB 截图、渲染一帧并退出 | 通过 |

离线使用两个 netif 和真实的 lwIP TCP 客户端/服务端，在包队列中注入丢失和重排。
虚拟单调时钟驱动 sys_check_timeouts，定时器测试不需要等待真实的重传间隔。
发送缓冲、接收回调和错误回调都使用 raw API。UDP 两端也都使用 lwIP，
该往返结果不能代替 RTCP 发到 Apple 媒体 socket 的真机验证。

## 本轮发现并修正的问题

- 64 位主机的 IPv6 重组辅助结构需要 `IPV6_FRAG_COPYHEADER=1`。
  原配置能编译但会在 lwip_init 的断言处终止，运行测试后才暴露。
- RSD 的 Port 可能是字符串。探针使用有范围及完整字符串校验的数值转换。
- 隧道可读等待的普通超时不能当成连接失败。Socket / PacketTunnel 增加可选的
  timed_out 返回值；旧调用方的错误文本和行为保持原样。新增回环测试覆盖超时、
  数据就绪、已关闭 fd 和旧调用约定。
- tcp_err 回调发生时 PCB 已释放；探针清空指针。成功 tcp_close 后不再访问 PCB，
  且预先解绑对象回调，避免已销毁的 Connection 被后续 FIN/ACK 使用。

## 生产接入仍需完成的工作

1. 统一 lwIP 执行上下文。探针的 NO_SYS 模式只有一个主线程；生产中有多个应用线程、
   隧道读线程和 TCP/UDP 连接，不能在这些线程中直接并发调用 raw API。
   应建立一个统一的 lwIP 核心执行线程和请求/结果交接，所有 netif、PCB、收包和定时器
   都由它管理。lwip_init 只执行一次；不能把探针 Engine 按每台 Device 分别创建。
2. 分离网络回调与隧道 I/O。output_ip6 只排队，回调不执行 TLS 阻塞写。
   生产需要处理发送队列上限、隧道错误传播、关闭时取消等待和 worker 回收。
   每个 IPv6 包仍须单独发送，保持现有隧道的包边界。
3. 将 ByteStream 适配为当前调用方期望的同步读写，定义发送超时、缓冲上限与半关闭行为。
   探针有 16 MiB 应用接收上限和独立发送期限；这些参数尚不是生产策略。
4. 将 UDP 端点、ICMP 观察与统计一起纳入迁移，保留媒体恢复依赖的行为。
   当前 TCP 丢段计数不能直接等同于 lwIP 的乱序队列或重传计数，统计含义需要重新定义。
5. 覆盖 USB / Wi-Fi 媒体、真实断线与反复重连、退出和多设备。
   完成后再删除旧 TCP 和包分发实现，避免长期维护两套生产后端。

本轮不证明长期媒体运行、实际 RTCP 投递或物理断线恢复。USB 与用户提供的酒店
Wi-Fi 地址均成功建立会话；仅证明当前设备间路径可用，不代表其他酒店网络也能互访。
64 位 IPv6 分片重组配置已修正，但没有注入 IPv6 分片来验证完整重组行为。
测试用 ASan 检查了内存访问，不等同于完整泄漏检查。
真实截图是否使用 FileTransfer 取决于设备版本，本轮取得的截图为内联 Data。

## 复现

```sh
cmake -S . -B build-lwip -DSCRCTL_LWIP_PROBE=ON
cmake --build build-lwip -j
ctest --test-dir build-lwip --output-on-failure
./build-lwip/lwip_probe --usb
# 已完成远程配对、设备能经局域网访问时：
./build-lwip/lwip_probe --wifi <设备地址>
```

默认选项为 OFF，正常构建不下载、不链接 lwIP。启用时从上游固定标签归档获取源码，
校验 SHA256；离线可用 `FETCHCONTENT_SOURCE_DIR_LWIP` 指定已解压的同版本源码。
库配置决定 ABI，不能直接换成配置未知的系统 lwIP 二进制包。
探针输出仅包含测试结果、服务数量、MTU 和图片长度，不保存图片、原始隧道包或密钥。

## 参考

- [lwIP 2.2.1 上游源码](https://github.com/lwip-tcpip/lwip/tree/STABLE-2_2_1_RELEASE)
- [Raw API 与线程约束](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/doc/doxygen/main_page.h)
- [TCP API、close 与回调实现](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp.c)
- [IPv6 分片配置](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/include/lwip/ip6_frag.h)
