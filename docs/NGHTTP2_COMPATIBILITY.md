# nghttp2 适配验证

验证日期：2026-10-03。库版本：1.70.0。设备：iPhone14,4 / iOS 27.0，使用热点 Wi-Fi。

## 结论

nghttp2 能处理 RemoteXPC 的空 HEADERS、奇数号双向控制流和大型内联回复。
但原有 Channel::receive_file 会由客户端在偶数号流上发送 HEADERS；nghttp2 的
客户端会话不允许以这种方式建立流。关闭 HTTP 消息校验不会取消客户端/服务端模型。
因此这次不替换生产传输，也不增加两套 HTTP/2 后端。

这项结论针对目前的文件流约定，并非断言 nghttp2 无法用于所有 Apple 服务。
若以后调整文件流接入方式，需要重新验证协议兼容性及复杂度收益。

## 验证结果

| 范围 | 方法 | 结果 |
| --- | --- | --- |
| 空 HEADERS、流 1/3 | 离线检查库生成的帧和双向 DATA | 通过 |
| 发送流控 | 150000 字节 DATA，耗尽默认窗口后补 WINDOW_UPDATE | 暂停与恢复均正常 |
| 接收流控 | 接收 20 MiB，超过 16 MiB 初始窗口 | 库补充连接及流窗口，字节完整 |
| 握手和服务调用 | 真机 getmediasupportinfo | 通过，使用 nghttp2 生成握手和请求帧 |
| 大型内联截图 | 真机 capturescreenshot | 返回 4159838 字节 PNG，签名正确 |
| 入站分片 | 生产实现捕获的目录和截图记录，以 1 / 17 / 65536 字节分块重放 | 字节计数一致，无非法帧 |
| 客户端偶数号 HEADERS | 离线尝试将下一个流设为 6，并提交流 2 HEADERS | 设置被拒绝，HEADERS 未发送 |
| 未经 PUSH_PROMISE 的服务端偶数流 | 离线注入流 2 HEADERS / DATA | 协议错误，文件 DATA 未交付 |
| 真机 FileTransfer 子流 | 当前设备截图实际返回内联 Data | 未触发，不能声称已做真机文件子流验证 |

旧注释把截图固定描述为 FileTransfer，与当前设备行为不符，已修改。
原始目录及截图记录在私有临时目录中完成重放，未加入仓库，验证后已清理。

## 复现

可选探针默认关闭，生产目标不链接 nghttp2。启用时需要 pkg-config 和系统 libnghttp2 >= 1.50；
本次只验证了 1.70.0，不保证其他版本具有相同结果。

```sh
cmake -S . -B /tmp/scrctl-nghttp2-build -DSCRCTL_NGHTTP2_PROBE=ON
cmake --build /tmp/scrctl-nghttp2-build -j
ctest --test-dir /tmp/scrctl-nghttp2-build --output-on-failure
/tmp/scrctl-nghttp2-build/experiments/nghttp2_probe
/tmp/scrctl-nghttp2-build/experiments/nghttp2_probe --wifi <device-ip>
/tmp/scrctl-nghttp2-build/experiments/nghttp2_probe --wifi <device-ip> --screenshot
/tmp/scrctl-nghttp2-build/experiments/nghttp2_probe --replay <channel-input.bin>
```

无参数探针和重放返回 0 表示预期观察与断言通过，不表示完整兼容。
真机模式返回 0 表示服务调用或内联截图成功；遇到 FileTransfer 描述并确认接受帧无法发送时返回 2。
其他错误返回 1。探针不输出 XPC 内容、图片像素或配对密钥。

## 上游说明

- [no_http_messaging](https://nghttp2.org/documentation/nghttp2_option_set_no_http_messaging.html)：关闭消息规则校验，仍保留客户端/服务端模型。
- [set_next_stream_id](https://nghttp2.org/documentation/nghttp2_session_set_next_stream_id.html)：客户端不能将下一个流设为偶数。
- [submit_headers](https://nghttp2.org/documentation/nghttp2_submit_headers.html)：新请求流与既有流的提交规则不同。
