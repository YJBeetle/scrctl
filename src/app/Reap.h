#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace scrctl::app {

/// 回收已经退出 worker 的画面源，返回本次回收数量。其析构会 join，
/// 但 worker 已结束，因此调用方无需等待进行中的截图 RPC。
/// 切回视频时先请求停止，再将截图源放入退役容器；每帧清理完成的源，
/// 避免渲染线程阻塞，也避免反复切换后保留多个约 11 MiB 的 BGRA 帧。
/// 此规则使用模板以便用假源离线验证。
template <class T, class Done>
std::size_t reap_finished(std::vector<std::unique_ptr<T>> &retired, Done &&done) {
    const std::size_t before = retired.size();
    std::erase_if(retired, [&](const std::unique_ptr<T> &p) { return done(*p); });
    return before - retired.size();
}

}  // namespace scrctl::app
