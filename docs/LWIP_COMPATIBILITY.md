# lwIP 隧道适配验证

## 当前结论（2026-10-07）

生产 Stack / TcpStream / UdpSocket 已使用 lwIP 2.2.1。原自写 TCP 收发与
手工 UDP 封包、分发实现已删除。独立探针和生产适配层分别完成了协议及线程测试；
USB / Wi-Fi 上的截图、持续视频和反复截图切换也通过。

RemoteXPC 依赖 ByteStream 的 send / recv 接口，生产和探针复用同一份 HTTP/2、
XPC、身份申报和文件处理代码。

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
| 生产 USB 视频 | 运行 35 秒，705 帧，SR / RR 持续增长，无自动重起 | 通过 |
| 生产 Wi-Fi 切换 | 运行 30 秒，406 帧，两轮实时流 / 截图切换，正常退出 | 通过 |
| 生产音视频与内存访问 | ASan 下 USB 运行 25 秒，512 帧，两轮切换，音频解码并交付 dummy 声卡 | 通过；不证明扬声器实际播放 |
| 生产线程 | 同步端点、多 netif、丢包、慢读者、阻塞发送、EOF、反复销毁及取消建连 / TCP读 / UDP读 | 通过 |
| 生产检查器 | ASan 的网络测试；TSan 的生产适配测试 | 通过，未报告内存访问错误或数据竞争 |

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

## 生产线程与关闭边界

- 全进程只初始化一次 lwIP。统一核心线程串行执行 netif、PCB、收包及定时器操作，
  同步任务队列最多 1024 项；任务不执行阻塞网络 I/O，也不等待应用取数据。
- 每条隧道一个 I/O 线程独占 TLS 读写。output_ip6 复制完整包到最多 4 MiB 的出站队列。
  每轮最多发送 64 包，再处理入站；各 IPv6 包仍单独写入隧道。
- TCP 建连、发送各最多等待 15 秒。发送串行化；接收上限 4 MiB，应用取数据后
  才调用 tcp_recved，因此慢读者会压缩对端可用窗口。
- UDP 使用 lwIP 的组包和校验和，队列最多 4096 包 / 16 MiB，满时丢最旧的包。
  只接收配置对端地址，出站超过 MTU 的数据报直接拒绝。
- 停止先中断底层 I/O、唤醒端点等待，再回收隧道线程与 netif。
  tcp_err 发生时 PCB 已释放；FIN 后的 PCB 用 ext-arg 销毁回调跟踪，
  netif 移除前中止尚未释放的 PCB，避免后续定时器访问已销毁的隧道。
- TCP 统计表示交付给应用缓冲的字节，替代旧的乱序丢弃指标。

## 尚未证明的行为

本轮验证了短时媒体和心跳持续收发，以及测试注入的关闭 / 取消。尚未验证物理拔插、
长时间媒体、多台真机并发、实际无线断线重连和扬声器播放。
酒店 Wi-Fi 地址成功建立会话，仅说明本次设备间路径可用。
IPv6 重组已启用，但没有注入分片验证完整重组。ASan 结果不等同于泄漏检查。
真实截图是否使用 FileTransfer 取决于设备版本，本轮取得的截图为内联 Data。

## 复现

```sh
cmake -S . -B build-lwip -DSCRCTL_LWIP_PROBE=ON
cmake --build build-lwip -j
ctest --test-dir build-lwip --output-on-failure
./build-lwip/experiments/lwip_probe --usb
# 已完成远程配对、设备能经局域网访问时：
./build-lwip/experiments/lwip_probe --wifi <设备地址>
```

独立探针默认 OFF；生产始终使用 lwIP。构建从上游固定标签归档获取源码并
校验 SHA256；离线可用 `FETCHCONTENT_SOURCE_DIR_LWIP` 指定已解压的同版本源码。
库配置决定 ABI，不能直接换成配置未知的系统 lwIP 二进制包。
探针输出仅包含测试结果、服务数量、MTU 和图片长度，不保存图片、原始隧道包或密钥。

## 参考

- [lwIP 2.2.1 上游源码](https://github.com/lwip-tcpip/lwip/tree/STABLE-2_2_1_RELEASE)
- [Raw API 与线程约束](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/doc/doxygen/main_page.h)
- [TCP API、close 与回调实现](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp.c)
- [IPv6 分片配置](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/include/lwip/ip6_frag.h)
