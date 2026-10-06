// 传输层 SIGPIPE 防护自检。
//
// 钉的是 SIGPIPE：对端 RST/半关闭之后写一次的**默认动作是杀进程**，不是返回错误。
// 真机上的表现是"拔线/设备睡觉时 scrctl 无声退出"，日志里什么都没有，只能靠 core 猜，
// 所以离线把它逼出来比在设备上复现便宜得多。
//
// 有**两条**写路径，防护机制不同，必须分开判：
//   1. 我们自己的 `Socket::write_all` —— send 带 MSG_NOSIGNAL（Linux 一直有；macOS 27
//      的 SDK 也开始定义了，实测 0x80000），fd 上的 SO_NOSIGPIPE 再兜一层。
//   2. OpenSSL 的 socket BIO —— `SSL_set_fd` 之后是它自己的裸 `write()`，拿不到任何
//      send 标志，**只有 fd 上的 SO_NOSIGPIPE 或进程级 SIG_IGN 盖得住**。usbmux 交出来
//      的那条 AF_UNIX 隧道（lockdown→TLS 用的正是它）曾经两样都没有（审查 P1）。
// 第 2 条才是这里要抓的，所以用裸 write()、并且在 fork 出来的子进程里做：防护失效时给
// 一条明确的 FAIL，而不是让整个测试被信号打死、后面的判据全看不到。
//
// 一个实测事实决定了测试的写法：macOS 上**对端已经关掉时** setsockopt(SO_NOSIGPIPE)
// 返回 EINVAL（选项设不上），所以必须"先接管 fd、后关对端"——这也正是生产路径的顺序
// （usbmux open、connect_tcp 都是在连接活着时接管）。反过来，这个 EINVAL 也说明 fd 级
// 那层不是万无一失，进程级 SIG_IGN 必须无条件装上。
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "transport/TlsChannel.h"
#include "transport/Usbmux.h"

namespace {

int Failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++Failures;
    }
}

/// 一条 socketpair，两端各自交给一个 Socket。
bool make_pair(scrctl::transport::Socket &a, scrctl::transport::Socket &b, std::string &err) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        err = std::string("socketpair 失败: ") + std::strerror(errno);
        return false;
    }
    a.reset(fds[0]);
    b.reset(fds[1]);
    return true;
}

/// 一条"对端已经关掉"的 socket，交给 `out`。`via_reset` 选接管入口：
/// usbmux 那条隧道是 open→release→reset 这么交出来的，TcpConnect 那条是构造交的，
/// 两个入口都得把该设的选项设上。
bool orphan_socket(scrctl::transport::Socket &out, bool via_reset, std::string &err) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        err = std::string("socketpair 失败: ") + std::strerror(errno);
        return false;
    }
    if (via_reset) {
        out.reset(fds[0]);
    } else {
        out = scrctl::transport::Socket(fds[0]);
    }
    ::close(fds[1]);  // 对端走掉：此后这一端的写就是 SIGPIPE 的触发条件
    return true;
}

/// 在子进程里对 `fd` 做一次**裸 write()**（OpenSSL 的 socket BIO 就是这么写的），
/// 判"没被 SIGPIPE 打死，而且拿到了 EPIPE"。
bool bare_write_survives(int fd) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const ssize_t n = ::write(fd, "ping", 4);
        ::_exit(n < 0 && errno == EPIPE ? 0 : 1);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return false;
    }
    if (WIFSIGNALED(status)) {
        std::printf("        子进程被信号 %d 打死%s\n", WTERMSIG(status),
                    WTERMSIG(status) == SIGPIPE ? "（就是 SIGPIPE）" : "");
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void own_send_path() {
    std::printf("\n我们自己的 write_all（send 带 MSG_NOSIGNAL）\n");
    for (const bool via_reset : {false, true}) {
        scrctl::transport::Socket s;
        std::string err;
        if (!orphan_socket(s, via_reset, err)) {
            check(false, err);
            continue;
        }
        // 写两次而不判第一次：AF_UNIX 上对端 close 之后第一次写通常就 EPIPE，但
        // "第一次被缓冲下来"也是合法实现，判据只该钉"不会一直成功"。
        const bool first = s.write_all("ping", 4, err);
        const bool second = s.write_all("ping", 4, err);
        check(!first || !second,
              std::string(via_reset ? "reset()" : "构造") + " 接管的 fd：写失败而非杀进程: " + err);
    }
}

void tls_write_path() {
    std::printf("\nOpenSSL 那条裸 write()（socket BIO 走的路）\n");
    scrctl::transport::Socket sock;
    std::string err;
    if (!orphan_socket(sock, /*via_reset=*/true, err)) {
        check(false, err);
        return;
    }
#ifdef SO_NOSIGPIPE
    // fd 级那层：由 Socket 接管时设。usbmux 绕过 TcpConnect 自己建 fd，曾经从这里漏出去。
    int value = 0;
    socklen_t len = sizeof(value);
    const int rc = ::getsockopt(sock.fd(), SOL_SOCKET, SO_NOSIGPIPE, &value, &len);
    check(rc == 0 && value == 1, "接管过的 fd 上确实设了 SO_NOSIGPIPE");
#endif
    // 进程级那层：handshake_psk 传空 PSK 会立刻失败返回，但装防护那一行已经跑过——
    // 这正是生产路径的顺序（先装防护，再碰对端）。
    scrctl::transport::TlsChannel tls;
    std::string herr;
    check(!tls.handshake_psk(sock, {}, herr) && !herr.empty(),
          "空 PSK 握手按预期失败（只为跑过装防护那一行）: " + herr);
#ifdef SIGPIPE
    struct sigaction old {};
    ::sigaction(SIGPIPE, nullptr, &old);
    check(old.sa_handler == SIG_IGN, "握手之后进程级 SIGPIPE 已是 SIG_IGN（不分平台）");
#endif
    check(bare_write_survives(sock.fd()), "裸 write() 不被 SIGPIPE 打死");
}

void still_works_as_a_socket() {
    std::printf("\n设选项没把 socket 设坏\n");
    scrctl::transport::Socket a;
    scrctl::transport::Socket b;
    std::string err;
    if (!make_pair(a, b, err)) {
        check(false, err);
        return;
    }
    check(a.write_all("hello", 5, err), "写: " + err);
    char buf[8] = {0};
    check(b.read_exact(buf, 5, err), "读: " + err);
    check(std::string(buf, 5) == "hello", "字节原样往返");
}

void readable_wait_results() {
    scrctl::transport::Socket a, b;
    std::string err;
    if (!make_pair(a, b, err)) {
        check(false, err);
        return;
    }
    bool timed_out = false;
    err = "previous error";
    check(!b.wait_readable(0, err, &timed_out) && timed_out && err.empty(),
          "空队列：等待超时，与传输错误区分");
    check(a.write_all("x", 1, err), "准备可读数据");
    timed_out = true;
    check(b.wait_readable(100, err, &timed_out) && !timed_out,
          "数据到达：清除超时标志");
    char byte = 0;
    check(b.read_exact(&byte, 1, err), "取走可读数据");
    check(!b.wait_readable(0, err) && err == "等待超时",
          "未提供超时标志时保留原有错误语义");
    b.close();
    timed_out = true;
    check(!b.wait_readable(0, err, &timed_out) && !timed_out && !err.empty(),
          "本地已关闭：返回错误原因，不标记超时");
}

}  // namespace

int main() {
    std::printf("== 传输层 SIGPIPE 防护 ==\n");
    own_send_path();
    tls_write_path();
    still_works_as_a_socket();
    readable_wait_results();
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
