#pragma once
#include <string>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

// 网络工具函数：同时被 networking.cpp 和 net.cpp 使用，统一处理 IPv4 / IPv6 / 域名解析与带超时的连接
namespace net {
    namespace detail {
#ifdef _WIN32
        typedef SOCKET RawSock;
        inline int lastSockError() { return WSAGetLastError(); }
        inline bool isInProgress(int err) { return err == WSAEWOULDBLOCK; }
        inline bool isTimeout(int err) { return err == WSAETIMEDOUT; }
        inline bool isRefused(int err) { return err == WSAECONNREFUSED; }
        inline bool isUnreachable(int err) { return err == WSAENETUNREACH || err == WSAEHOSTUNREACH || err == WSAEADDRNOTAVAIL || err == WSAEAFNOSUPPORT; }
        const int kTimedOut = WSAETIMEDOUT;
#else
        typedef int RawSock;
        inline int lastSockError() { return errno; }
        inline bool isInProgress(int err) { return err == EINPROGRESS; }
        inline bool isTimeout(int err) { return err == ETIMEDOUT; }
        inline bool isRefused(int err) { return err == ECONNREFUSED; }
        inline bool isUnreachable(int err) { return err == ENETUNREACH || err == EHOSTUNREACH || err == EADDRNOTAVAIL || err == EAFNOSUPPORT; }
        const int kTimedOut = ETIMEDOUT;
#endif

        // 去掉首尾空白（手机输入法可能带入空格/换行），并去掉 IPv6 字面量外层的方括号："[::1]" -> "::1"
        inline std::string normalizeHost(const std::string& in) {
            size_t b = in.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) { return ""; }
            size_t e = in.find_last_not_of(" \t\r\n");
            std::string s = in.substr(b, e - b + 1);
            if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
                s = s.substr(1, s.size() - 2);
            }
            return s;
        }

        // 解析主机名，同时返回 IPv6 和 IPv4 地址（调用者负责 freeaddrinfo）。
        // 先使用 AI_ADDRCONFIG，只返回本机已配置的协议族；若无结果再不带该标志重试。
        inline struct addrinfo* resolveHost(const std::string& host, int port, int socktype, int family = AF_UNSPEC) {
            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = family;
            hints.ai_socktype = socktype;
            std::string portStr = std::to_string(port);

            struct addrinfo* res = NULL;
            hints.ai_flags = AI_ADDRCONFIG;
            if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) == 0 && res) { return res; }
            if (res) { freeaddrinfo(res); res = NULL; }

            hints.ai_flags = 0;
            if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) == 0 && res) { return res; }
            if (res) { freeaddrinfo(res); }
            return NULL;
        }

        inline void setBlockingMode(RawSock sock, bool blocking) {
#ifdef _WIN32
            u_long nb = blocking ? 0 : 1;
            ioctlsocket(sock, FIONBIO, &nb);
#else
            int flags = fcntl(sock, F_GETFL, 0);
            if (flags < 0) { return; }
            fcntl(sock, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
        }

        // 带超时的 TCP 连接。成功返回 true；失败返回 false 并通过 err 返回错误码。
        // 返回时套接字保持阻塞模式，由调用者决定是否切换为非阻塞。
        inline bool connectWithTimeout(RawSock sock, const struct sockaddr* sa, int len, int timeoutMs, int& err) {
            setBlockingMode(sock, false);

            bool ok = (::connect(sock, sa, (socklen_t)len) == 0);
            if (!ok) {
                int e = lastSockError();
                if (!isInProgress(e)) {
                    err = e;
                }
                else {
                    bool ready = false;
#ifdef _WIN32
                    fd_set wfds, efds;
                    FD_ZERO(&wfds);
                    FD_ZERO(&efds);
                    FD_SET(sock, &wfds);
                    FD_SET(sock, &efds);
                    struct timeval tv;
                    tv.tv_sec = timeoutMs / 1000;
                    tv.tv_usec = (timeoutMs % 1000) * 1000;
                    int sel = select(0, NULL, &wfds, &efds, &tv);
                    ready = (sel > 0);
#else
                    struct pollfd pfd;
                    pfd.fd = sock;
                    pfd.events = POLLOUT;
                    pfd.revents = 0;
                    int sel = poll(&pfd, 1, timeoutMs);
                    ready = (sel > 0);
#endif
                    if (!ready) {
                        err = kTimedOut;
                    }
                    else {
                        int soerr = 0;
                        socklen_t soerrLen = sizeof(soerr);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerrLen);
                        if (soerr == 0) { ok = true; }
                        else { err = soerr; }
                    }
                }
            }

            setBlockingMode(sock, true);
            return ok;
        }

        // 把连接错误码转换成给用户看的中文说明
        inline std::string describeConnectError(int err) {
            if (isTimeout(err)) { return "连接超时，服务器无响应或被防火墙拦截"; }
            if (isRefused(err)) { return "连接被拒绝，服务器未在该端口监听"; }
            if (isUnreachable(err)) { return "网络或主机不可达，请检查网络（IPv6 地址需要本机和服务器都有可用的 IPv6 网络）"; }
            return "连接失败，错误码 " + std::to_string(err);
        }
    }
}
