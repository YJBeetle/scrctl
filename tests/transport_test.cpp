// POSIX 传输回归：普通 send 与 OpenSSL socket BIO 分别检查 SIGPIPE 防护。
// 子进程保留默认 SIGPIPE 行为，便于将缺失防护明确报告为失败。
// 先接管活连接、再关闭对端，保持与生产创建和转发连接的顺序一致。
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "transport/TlsChannel.h"
#include "tls_psk_move.h"
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
    check(!b.wait_readable(0, err) && err == "Wait timed out",
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
    scrctl_test::tls_failures(check);
    scrctl_test::tls_psk_moves(check);
    std::printf("\n%s (失败 %d 项)\n", Failures == 0 ? "全部通过" : "存在失败", Failures);
    return Failures == 0 ? 0 : 1;
}
