#include "net.h"
#include "net_util.h"
#include <algorithm>
#include <utils/flog.h>
#include <string.h>
#include <codecvt>
#include <stdexcept>

#ifdef _WIN32
#define WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#define WOULD_BLOCK (errno == EWOULDBLOCK)
#endif

namespace net {
    bool _init = false;
    
    // === Private functions ===

    void init() {
        if (_init) { return; }
#ifdef _WIN32
        // Initialize WinSock2
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
            throw std::runtime_error("Could not initialize WinSock2");
            return;
        }
#else
        // Disable SIGPIPE to avoid closing when the remote host disconnects
        signal(SIGPIPE, SIG_IGN);
#endif
        _init = true;
    }

    bool queryHost(uint32_t* addr, std::string host) {
        hostent* ent = gethostbyname(host.c_str());
        if (!ent || !ent->h_addr_list[0]) { return false; }
        *addr = *(uint32_t*)ent->h_addr_list[0];
        return true;
    }

    void closeSocket(SockHandle_t sock) {
#ifdef _WIN32
        shutdown(sock, SD_BOTH);
        closesocket(sock);
#else
        shutdown(sock, SHUT_RDWR);
        close(sock);
#endif
    }

    void setNonblocking(SockHandle_t sock) {
#ifdef _WIN32
        u_long enabled = 1;
        ioctlsocket(sock, FIONBIO, &enabled);
#else
        fcntl(sock, F_SETFL, O_NONBLOCK);
#endif
    }

    // === Address functions ===

    Address::Address() {
        memset(&addr, 0, sizeof(addr));
        addr.ss_family = AF_INET;
    }

    Address::Address(const std::string& host, int port) {
        // Initialize WSA if needed
        init();

        // 支持 IPv4 地址、IPv6 地址（可带方括号）和域名
        std::string target = detail::normalizeHost(host);
        struct addrinfo* results = target.empty() ? NULL : detail::resolveHost(target, port, SOCK_STREAM);
        if (!results) {
            throw std::runtime_error("Unknown host");
        }

        // 使用解析结果中的第一个地址（顺序遵循系统策略，通常 IPv6 优先）
        memset(&addr, 0, sizeof(addr));
        memcpy(&addr, results->ai_addr, std::min<size_t>((size_t)results->ai_addrlen, sizeof(addr)));
        freeaddrinfo(results);
    }

    Address::Address(IP_t ip, int port) {
        memset(&addr, 0, sizeof(addr));
        struct sockaddr_in* a4 = (struct sockaddr_in*)&addr;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(ip);
        a4->sin_port = htons(port);
    }

    std::string Address::getIPStr() const {
        char buf[128];
        if (isIPv6()) {
            const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)&addr;
            if (!inet_ntop(AF_INET6, (const void*)&a6->sin6_addr, buf, sizeof(buf))) { return "::"; }
            return buf;
        }
        IP_t ip = getIP();
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        return buf;
    }

    IP_t Address::getIP() const {
        // 仅对 IPv4 地址有意义，IPv6 地址返回 0
        if (isIPv6()) { return 0; }
        return ntohl(((const struct sockaddr_in*)&addr)->sin_addr.s_addr);
    }

    void Address::setIP(IP_t ip) {
        // 设置 IPv4 地址（如果原来是 IPv6，会转换为 IPv4 并保留端口）
        int port = getPort();
        memset(&addr, 0, sizeof(addr));
        struct sockaddr_in* a4 = (struct sockaddr_in*)&addr;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(ip);
        a4->sin_port = htons(port);
    }

    int Address::getPort() const {
        if (isIPv6()) { return ntohs(((const struct sockaddr_in6*)&addr)->sin6_port); }
        return ntohs(((const struct sockaddr_in*)&addr)->sin_port);
    }

    void Address::setPort(int port) {
        if (isIPv6()) { ((struct sockaddr_in6*)&addr)->sin6_port = htons(port); }
        else { ((struct sockaddr_in*)&addr)->sin_port = htons(port); }
    }

    bool Address::isIPv6() const {
        return addr.ss_family == AF_INET6;
    }

    int Address::family() const {
        return isIPv6() ? AF_INET6 : AF_INET;
    }

    int Address::size() const {
        return isIPv6() ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    }

    bool Address::isUnspecified() const {
        if (isIPv6()) {
            const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)&addr;
            return IN6_IS_ADDR_UNSPECIFIED(&a6->sin6_addr);
        }
        return ((const struct sockaddr_in*)&addr)->sin_addr.s_addr == 0;
    }

    // === Socket functions ===

    Socket::Socket(SockHandle_t sock, const Address* raddr) {
        this->sock = sock;
        if (raddr) {
            this->raddr = new Address(*raddr);
        }
    }

    Socket::~Socket() {
        close();
        if (raddr) { delete raddr; }
    }

    void Socket::close() {
        if (!open) { return; }
        open = false;
        closeSocket(sock);
    }

    bool Socket::isOpen() {
        return open;
    }

    SocketType Socket::type() {
        return raddr ? SOCKET_TYPE_UDP : SOCKET_TYPE_TCP;
    }

    int Socket::send(const uint8_t* data, size_t len, const Address* dest) {
        int err;
        if (!dest && !raddr) {
            err = ::send(sock, (const char*)data, len, 0);
        } else {
            const Address* target = dest ? dest : raddr;
            err = sendto(sock, (const char*)data, len, 0, (sockaddr*)&target->addr, target->size());
        }

        // On error, close socket
        if (err <= 0 && !WOULD_BLOCK) {
            close();
            return err;
        }

        return err;
    }

    int Socket::sendstr(const std::string& str, const Address* dest) {
        return send((const uint8_t*)str.c_str(), str.length(), dest);
    }

    int Socket::recv(uint8_t* data, size_t maxLen, bool forceLen, int timeout, Address* dest) {
        // Create FD set
        fd_set set;
        FD_ZERO(&set);
        
        int read = 0;
        bool blocking = (timeout != NONBLOCKING);
        do {
            // Wait for data or error if 
            if (blocking) {
                // Enable FD in set
                FD_SET(sock, &set);

                // Set timeout
                timeval tv;
                tv.tv_sec = timeout / 1000;
                tv.tv_usec = (timeout - tv.tv_sec*1000) * 1000;

                // Wait for data
                int err = select(sock+1, &set, NULL, &set, (timeout > 0) ? &tv : NULL);
                if (err <= 0) { return err; }
            }

            int err;
            // 【修复】如果是 TCP 接收，直接使用 recv()
            if (!dest) {
                err = ::recv(sock, (char*)&data[read], maxLen - read, 0);
            } else {
                // 如果是 UDP，使用 recvfrom 和通用的 sockaddr_storage
                int addrLen = sizeof(struct sockaddr_storage);
                err = ::recvfrom(sock, (char*)&data[read], maxLen - read, 0, (sockaddr*)&dest->addr, (socklen_t*)&addrLen);
            }

            if (err <= 0 && !WOULD_BLOCK) {
                close();
                return err;
            }
            read += err;
        }
        while (blocking && forceLen && read < maxLen);
        return read;
    }

    int Socket::recvline(std::string& str, int maxLen, int timeout, Address* dest) {
        // Disallow nonblocking mode
        if (!timeout) { return -1; }
        
        str.clear();
        int read = 0;
        while (!maxLen || read < maxLen) {
            char c;
            int err = recv((uint8_t*)&c, 1, false, timeout, dest);
            if (err <= 0) { return err; }
            read++;
            if (c == '\n') { break; }
            str += c;
        }
        return read;
    }

    // === Listener functions ===

    Listener::Listener(SockHandle_t sock) {
        this->sock = sock;
    }

    Listener::~Listener() {
        stop();
    }

    void Listener::stop() {
        closeSocket(sock);
        open = false;
    }

    bool Listener::listening() {
        return open;
    }

    std::shared_ptr<Socket> Listener::accept(Address* dest, int timeout) {
        // Create FD set
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock, &set);

        // Define timeout
        timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout - tv.tv_sec*1000) * 1000;

        // Wait for data or error
        if (timeout != NONBLOCKING) {
            int err = select(sock+1, &set, NULL, &set, (timeout > 0) ? &tv : NULL);
            if (err <= 0) { return NULL; }
        }

        // Accept
        int addrLen = sizeof(struct sockaddr_storage);
        SockHandle_t s = ::accept(sock, (sockaddr*)(dest ? &dest->addr : NULL), (socklen_t*)(dest ? &addrLen : NULL));
        if ((int)s < 0) {
            if (!WOULD_BLOCK) { stop(); }
            return NULL;
        }

        // Enable nonblocking mode
        setNonblocking(s);

        return std::make_shared<Socket>(s);
    }

    // === Creation functions ===

    std::map<std::string, InterfaceInfo> listInterfaces() {
        // Init library if needed
        init();

        std::map<std::string, InterfaceInfo> ifaces;
#ifdef _WIN32
        // Pre-allocate buffer
        ULONG size = sizeof(IP_ADAPTER_ADDRESSES);
        PIP_ADAPTER_ADDRESSES addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

        // Reallocate to real size
        if (GetAdaptersAddresses(AF_INET, 0, NULL, addresses, &size) == ERROR_BUFFER_OVERFLOW) {
            addresses = (PIP_ADAPTER_ADDRESSES)realloc(addresses, size);
            if (GetAdaptersAddresses(AF_INET, 0, NULL, addresses, &size)) {
                throw std::exception("Could not list network interfaces");
            }
        }

        // Save data
        std::wstring_convert<std::codecvt_utf8<wchar_t>> utfConv;
        for (auto iface = addresses; iface; iface = iface->Next) {
            InterfaceInfo info;
            auto ip = iface->FirstUnicastAddress;
            if (!ip || ip->Address.lpSockaddr->sa_family != AF_INET) { continue; }
            info.address = ntohl(*(uint32_t*)&ip->Address.lpSockaddr->sa_data[2]);
            info.netmask = ~((1 << (32 - ip->OnLinkPrefixLength)) - 1);
            info.broadcast = info.address | (~info.netmask);
            ifaces[utfConv.to_bytes(iface->FriendlyName)] = info;
        }
        
        // Free tables
        free(addresses);
#else
        // Get iface list
        struct ifaddrs* addresses = NULL;
        getifaddrs(&addresses);

        // Save data
        for (auto iface = addresses; iface; iface = iface->ifa_next) {
            if (!iface->ifa_addr || !iface->ifa_netmask) { continue; }
            if (iface->ifa_addr->sa_family != AF_INET) { continue; }
            InterfaceInfo info;
            info.address = ntohl(*(uint32_t*)&iface->ifa_addr->sa_data[2]);
            info.netmask = ntohl(*(uint32_t*)&iface->ifa_netmask->sa_data[2]);
            info.broadcast = info.address | (~info.netmask);
            ifaces[iface->ifa_name] = info;
        }

        // Free iface list
        freeifaddrs(addresses);
#endif

        return ifaces;
    }

    std::shared_ptr<Listener> listen(const Address& addr) {
        // Init library if needed
        init();

        // Create socket (IPv4 or IPv6 depending on the address)
        SockHandle_t s = socket(addr.family(), SOCK_STREAM, IPPROTO_TCP);
        // TODO: Support non-blockign mode

#ifdef _WIN32
        if (s == INVALID_SOCKET) {
#else
        if (s < 0) {
#endif
            throw std::runtime_error("Could not create socket (IP version not supported on this system)");
        }

#ifndef _WIN32
        // Allow port reusing if the app was killed or crashed
        // and the socket is stuck in TIME_WAIT state.
        // This option has a different meaning on Windows,
        // so we use it only for non-Windows systems
        int enable = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
            closeSocket(s);
            throw std::runtime_error("Could not configure socket");
            return NULL;
        }
#endif

        // IPv6 监听套接字同时接受 IPv4 连接（双栈）。Windows 默认仅 IPv6，需要显式关闭 V6ONLY
        if (addr.isIPv6()) {
#ifdef _WIN32
            DWORD v6only = 0;
#else
            int v6only = 0;
#endif
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
        }

        // Bind socket to the port
        if (bind(s, (sockaddr*)&addr.addr, addr.size())) {
            closeSocket(s);
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        // Enable listening
        if (::listen(s, SOMAXCONN) != 0) {
            throw std::runtime_error("Could start listening for connections");
            return NULL;
        }

        // Enable nonblocking mode
        setNonblocking(s);

        // Return listener class
        return std::make_shared<Listener>(s);
    }

    std::shared_ptr<Listener> listen(std::string host, int port) {
        return listen(Address(host, port));
    }

    std::shared_ptr<Socket> connect(const Address& addr) {
        // Init library if needed
        init();

        // Create socket (IPv4 or IPv6 depending on the address)
        SockHandle_t s = socket(addr.family(), SOCK_STREAM, IPPROTO_TCP);

        // Connect to server (with timeout)

#ifdef _WIN32
        if (s == INVALID_SOCKET) {
#else
        if (s < 0) {
#endif
            throw std::runtime_error("Could not create socket (IP version not supported on this system)");
        }
        int err = 0;
        if (!detail::connectWithTimeout(s, (sockaddr*)&addr.addr, addr.size(), 8000, err)) {
            closeSocket(s);
            throw std::runtime_error("Could not connect: " + detail::describeConnectError(err));
            return NULL;
        }

        // Enable nonblocking mode
        setNonblocking(s);

        // Return socket class
        return std::make_shared<Socket>(s);
    }

    std::shared_ptr<Socket> connect(std::string host, int port) {
        // Init library if needed
        init();

        // 支持 IPv4 地址、IPv6 地址（可带方括号）和域名
        std::string target = detail::normalizeHost(host);
        if (target.empty()) {
            throw std::runtime_error("Host is empty");
        }

        struct addrinfo* results = detail::resolveHost(target, port, SOCK_STREAM);
        if (!results) {
            throw std::runtime_error("Unknown host");
        }

        // 依次尝试解析出的每个地址（域名可能同时有 IPv6 和 IPv4），每个地址最多等待 8 秒
        int lastErr = 0;
        for (struct addrinfo* ai = results; ai; ai = ai->ai_next) {
            SockHandle_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#ifdef _WIN32
            if (s == INVALID_SOCKET) { lastErr = detail::lastSockError(); continue; }
#else
            if (s < 0) { lastErr = detail::lastSockError(); continue; }
#endif
            int err = 0;
            if (detail::connectWithTimeout(s, ai->ai_addr, (int)ai->ai_addrlen, 8000, err)) {
                freeaddrinfo(results);

                // Enable nonblocking mode
                setNonblocking(s);

                return std::make_shared<Socket>(s);
            }
            lastErr = err;
            closeSocket(s);
        }

        freeaddrinfo(results);
        throw std::runtime_error("Could not connect: " + detail::describeConnectError(lastErr));
    }

    std::shared_ptr<Socket> openudp(const Address& raddr_in, const Address& laddr_in, bool allowBroadcast) {
        // Init library if needed
        init();

        // 确定使用的协议族：远端地址是具体地址时跟随远端；远端为通配地址（0.0.0.0 或 ::）时跟随本地地址
        Address raddr = raddr_in;
        Address laddr = laddr_in;
        int family = (!raddr.isUnspecified()) ? raddr.family() : laddr.family();

        // 本地或远端的通配地址与所选协议族不一致时，转换成该协议族的通配地址并保留端口
        auto toFamily = [family](Address& a) {
            if (a.family() == family) { return; }
            if (!a.isUnspecified()) { throw std::runtime_error("Local and remote addresses use different IP versions"); }
            int port = a.getPort();
            memset(&a.addr, 0, sizeof(a.addr));
            if (family == AF_INET6) {
                struct sockaddr_in6* a6 = (struct sockaddr_in6*)&a.addr;
                a6->sin6_family = AF_INET6;
                a6->sin6_addr = in6addr_any;
                a6->sin6_port = htons(port);
            }
            else {
                struct sockaddr_in* a4 = (struct sockaddr_in*)&a.addr;
                a4->sin_family = AF_INET;
                a4->sin_addr.s_addr = htonl(INADDR_ANY);
                a4->sin_port = htons(port);
            }
        };
        toFamily(raddr);
        toFamily(laddr);

        // Create socket
        SockHandle_t s = socket(family, SOCK_DGRAM, IPPROTO_UDP);

#ifdef _WIN32
        if (s == INVALID_SOCKET) {
#else
        if (s < 0) {
#endif
            throw std::runtime_error("Could not create socket (IP version not supported on this system)");
        }

        // 广播只存在于 IPv4；IPv6 使用组播，不设置 SO_BROADCAST
        if (family == AF_INET) {
#ifdef _WIN32
            const char enable = allowBroadcast;
#else
            int enable = allowBroadcast;
#endif
            if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(int)) < 0) {
                closeSocket(s);
                throw std::runtime_error("Could not enable broadcast on socket");
                return NULL;
            }
        }
        else {
            // 双栈：同时接收 IPv4 映射地址的数据报
#ifdef _WIN32
            DWORD v6only = 0;
#else
            int v6only = 0;
#endif
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
        }

        // Bind socket to local port
        if (bind(s, (sockaddr*)&laddr.addr, laddr.size())) {
            closeSocket(s);
            throw std::runtime_error("Could not bind socket");
            return NULL;
        }

        // Return socket class
        return std::make_shared<Socket>(s, &raddr);
    }

    std::shared_ptr<Socket> openudp(std::string rhost, int rport, const Address& laddr, bool allowBroadcast) {
        return openudp(Address(rhost, rport), laddr, allowBroadcast);
    }

    std::shared_ptr<Socket> openudp(const Address& raddr, std::string lhost, int lport, bool allowBroadcast) {
        return openudp(raddr, Address(lhost, lport), allowBroadcast);
    }

    std::shared_ptr<Socket> openudp(std::string rhost, int rport, std::string lhost, int lport, bool allowBroadcast) {
        return openudp(Address(rhost, rport), Address(lhost, lport), allowBroadcast);
    }
}