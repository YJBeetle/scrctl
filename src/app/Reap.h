#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace scrctl::app {

/// 把退役表里"worker 已经退出"的那些摘掉。摘掉就是真销毁（析构里 join，而此时 join
/// 立刻返回），所以调用点不会等。
///
/// 为什么需要这一步：切回实时流发生在渲染线程上，那里不能 join 一个可能正卡在截图 RPC
/// 里的 worker（会把窗口冻到 RPC 上限），所以退下来的源先挂在表里。但每个都还揣着一整张
/// 解码好的 BGRA 画面（1125×2436 就是约 11 MB），一路挂到 teardown 的话，媒体流反复
/// 降级/回升就会按切换次数把内存堆上去（审查 P2）。回收的判据是"worker 自己说它退出了"，
/// 不是"等它退出"，所以这一趟永不阻塞；还没退的留到下一帧再看，最迟一次 RPC 上限后就能收。
///
/// 抽成模板函数与 `pick_picture_source` 同一个理由：真机上打不响"反复降级"这个触发器，
/// 规则本身得有离线判据（tests/app_test.cpp 用一个假的退役对象跑全组合）。
///
/// 返回这一趟摘掉了几个，只为日志/判据用。
template <class T, class Done>
std::size_t reap_finished(std::vector<std::unique_ptr<T>> &retired, Done &&done) {
    const std::size_t before = retired.size();
    std::erase_if(retired, [&](const std::unique_ptr<T> &p) { return done(*p); });
    return before - retired.size();
}

}  // namespace scrctl::app
