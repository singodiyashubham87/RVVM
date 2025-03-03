/*
networking.c - Network sockets (IPv4/IPv6), Event polling
Copyright (C) 2021  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifdef _WIN32

// Override maximum amount of sockets for select()
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h> // For IPv6

typedef SOCKET net_handle_t;
typedef int net_addrlen_t;
#define NET_HANDLE_INVALID INVALID_SOCKET

// Experimental WSAEventSelect implementation for net_poll
// Basically emulates epoll() on Win32. Needs testing!
//#define WSA_NET_IMPL

#else

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

typedef int net_handle_t;
typedef socklen_t net_addrlen_t;
#define NET_HANDLE_INVALID -1

#if defined(__linux__) || defined(__illumos__)
// Use epoll() for net_poll on Linux & Illumos
#include <sys/epoll.h>
#include <sys/resource.h>
#define EPOLL_NET_IMPL
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) \
   || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) \
   || (defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 1060)
// Use BSD kqueue() for net_poll
#include <sys/event.h>
#include <sys/resource.h>
#define KQUEUE_NET_IMPL
#endif

#endif

#if !(defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL) || defined(WSA_NET_IMPL))
// Use select() for net_poll
// Scales poorly, but it's a fairly portable fallback.
// Thread safety & other epoll-like features are well emulated.
#define SELECT_NET_IMPL
#endif

#ifdef AF_INET6
// Compile IPv6 support on systems where it's actually exposed
#define IPV6_NET_IMPL
#endif

#include "networking.h"
#include "utils.h"
#include "mem_ops.h"
#include "vector.h"
#include "hashmap.h"
#include "threading.h"
#include "spinlock.h"

#if defined(SELECT_NET_IMPL)
typedef struct {
    net_sock_t* sock;
    void*       data;
    uint32_t    flags;
} net_monitor_t;
#endif

#if defined(WSA_NET_IMPL)
typedef struct {
    net_poll_t* poll;
    void*       data;
    uint32_t    flags;
} net_watch_t;
#endif

struct net_sock {
#if defined(WSA_NET_IMPL)
    HANDLE event;
    HANDLE wait;
    vector_t(net_watch_t) watchers;
#elif defined(SELECT_NET_IMPL)
    vector_t(net_poll_t*) watchers;
#endif
    net_handle_t fd;
    net_addr_t   addr;
};

struct net_poll {
#if defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL)
    net_handle_t fd;
#elif defined(WSA_NET_IMPL)
    spinlock_t lock;
    hashmap_t  events;
    cond_var_t cond;
#else
    spinlock_t lock;
    vector_t(net_monitor_t) events;
    fd_set r_set,   w_set;
    fd_set r_ready, w_ready;
    int    max_fd;
    size_t consumed;
#endif
};

const net_addr_t net_ipv4_any_addr   = { .type = NET_TYPE_IPV4, };
const net_addr_t net_ipv4_local_addr = { .type = NET_TYPE_IPV4, .ip[0] = 127, .ip[3] = 1, };
const net_addr_t net_ipv6_any_addr   = { .type = NET_TYPE_IPV6, };
const net_addr_t net_ipv6_local_addr = { .type = NET_TYPE_IPV6, .ip[15] = 1, };

static void net_init_once()
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        rvvm_warn("Failed to initialize WinSock");
    }
#elif defined(SIGPIPE)
    // Ignore SIGPIPE (Do not crash on writes to closed socket)
    void (*handler)(int);
    handler = signal(SIGPIPE, SIG_IGN);
    if (handler != SIG_DFL) signal(SIGPIPE, handler);
#endif
#if defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL)
    struct rlimit rlim = {0};
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        if (rlim.rlim_cur < rlim.rlim_max && rlim.rlim_max > 1024) {
            rlim.rlim_cur = rlim.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rlim) == 0) {
                rvvm_info("Raising RLIMIT_NOFILE to %u", (uint32_t)rlim.rlim_cur);
            }
        }
    }
#endif
}

// Initialize networking automatically
static void net_init()
{
    DO_ONCE(net_init_once());
}

// Address types conversion (net_addr_t <-> sockaddr_in/sockaddr_in6)
static void net_sockaddr_from_addr(struct sockaddr_in* sock_addr, const net_addr_t* addr)
{
    memset(sock_addr, 0, sizeof(struct sockaddr_in));
    sock_addr->sin_family = AF_INET;
    if (addr) {
        write_uint16_be_m(&sock_addr->sin_port, addr->port);
        memcpy(&sock_addr->sin_addr.s_addr, addr->ip, 4);
    }
}

static void net_addr_from_sockaddr(net_addr_t* addr, const struct sockaddr_in* sock_addr)
{
    memset(addr, 0, sizeof(net_addr_t));
    addr->type = NET_TYPE_IPV4;
    addr->port = read_uint16_be_m(&sock_addr->sin_port);
    memcpy(addr->ip, &sock_addr->sin_addr.s_addr, 4);
}

#if defined(IPV6_NET_IMPL)
static void net_sockaddr6_from_addr(struct sockaddr_in6* sock_addr, const net_addr_t* addr)
{
    memset(sock_addr, 0, sizeof(struct sockaddr_in6));
    sock_addr->sin6_family = AF_INET6;
    write_uint16_be_m(&sock_addr->sin6_port, addr->port);
    memcpy(&sock_addr->sin6_addr.s6_addr, addr->ip, 16);
}

static void net_addr_from_sockaddr6(net_addr_t* addr, const struct sockaddr_in6* sock_addr)
{
    memset(addr, 0, sizeof(net_addr_t));
    addr->type = NET_TYPE_IPV6;
    addr->port = read_uint16_be_m(&sock_addr->sin6_port);
    memcpy(addr->ip, &sock_addr->sin6_addr.s6_addr, 16);
}
#endif

// Wrappers for generic operations on socket handles
static void net_close_handle(net_handle_t fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool net_handle_set_blocking(net_handle_t fd, bool block)
{
#ifdef _WIN32
    u_long blocking = block ? 0 : 1;
    return ioctlsocket(fd, FIONBIO, &blocking) == 0;
#elif defined(F_SETFL) && defined(O_NONBLOCK)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    flags = block ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0;
#else
    UNUSED(fd);
    if (!block) rvvm_warn("Non-blocking sockets are not supported on this OS");
    return false;
#endif
}

static void net_handle_set_cloexec(net_handle_t fd)
{
#if defined(_WIN32) && !defined(UNDER_CE)
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#elif defined(F_SETFD) && defined(FD_CLOEXEC)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
    UNUSED(fd);
#endif
}

// Set CLOEXEC flag on created sockets to prevent handle leaking
// Optimize nonblocking connects on modern Linux and *BSD
static net_handle_t net_socket_create_ex(int domain, int type, bool nonblock)
{
    net_handle_t fd = NET_HANDLE_INVALID;
#if defined(_WIN32) && !defined(UNDER_CE) && defined(WSA_FLAG_NO_HANDLE_INHERIT)
    fd = WSASocketW(domain, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    fd = socket(domain, type | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);
    if (fd != NET_HANDLE_INVALID) return fd;
#endif
    if (fd == NET_HANDLE_INVALID) {
        fd = socket(domain, type, 0);
        if (fd != NET_HANDLE_INVALID) net_handle_set_cloexec(fd);
    }
    if (nonblock && fd != NET_HANDLE_INVALID) net_handle_set_blocking(fd, false);
    return fd;
}

// Set CLOEXEC flag on accepted sockets, propagate blocking mode as on BSD stack
static net_handle_t net_accept_ex(net_handle_t listener, void* sock_addr, net_addrlen_t* addr_len)
{
    net_handle_t fd = NET_HANDLE_INVALID;
#ifdef __linux__
    // Linux accept(2) does not inherit nonblocking flag on created socket
    bool nonblock = !!(fcntl(listener, F_GETFL, 0) & O_NONBLOCK);
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK) && defined(__USE_GNU)
    fd = accept4(listener, sock_addr, addr_len, SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0));
#endif
#endif
    if (fd == NET_HANDLE_INVALID) {
        fd = accept(listener, sock_addr, addr_len);
        if (fd != NET_HANDLE_INVALID) {
            net_handle_set_cloexec(fd);
#ifdef __linux__
            if (nonblock) net_handle_set_blocking(fd, false);
#endif
        }
    }
    return fd;
}

static net_handle_t net_create_handle(int type, const net_addr_t* addr, bool nonblock)
{
    net_handle_t fd = NET_HANDLE_INVALID;
    net_init();
    if (addr == NULL || addr->type == NET_TYPE_IPV4) {
        fd = net_socket_create_ex(AF_INET, type, nonblock);
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
        fd = net_socket_create_ex(AF_INET6, type, nonblock);
#endif
    }
#if defined(IPPROTO_TCP) && defined(TCP_NODELAY)
    if (type == SOCK_STREAM && fd != NET_HANDLE_INVALID) {
        // Disable transmit buffering to improve latency, inherited in accept()
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const void*)&nodelay, sizeof(nodelay));
    }
#endif
    return fd;
}

static bool net_bind_handle(net_handle_t fd, const net_addr_t* addr)
{
    if (addr == NULL || addr->type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr;
        net_sockaddr_from_addr(&sock_addr, addr);
        return bind(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) == 0;
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
#if defined(SOL_IPV6) && defined(IPV6_V6ONLY)
        // Disable dual-stack explicitly (may be configurable in future)
        int v6only = 1;
        setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, (const void*)&v6only, sizeof(v6only));
#endif
        struct sockaddr_in6 sock_addr;
        net_sockaddr6_from_addr(&sock_addr, addr);
        return bind(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) == 0;
#endif
    }
    return false;
}

static bool net_conn_initiated()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

static bool net_connect_handle(net_handle_t fd, const net_addr_t* addr)
{
    if (addr == NULL || addr->type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr;
        net_sockaddr_from_addr(&sock_addr, addr);
        return connect(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) == 0
            || net_conn_initiated();
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr;
        net_sockaddr6_from_addr(&sock_addr, addr);
        return connect(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) == 0
            || net_conn_initiated();
#endif
    }
    return false;
}

// Wrap native handles in net_sock_t
static net_sock_t* net_wrap_handle(net_handle_t fd)
{
    if (fd == NET_HANDLE_INVALID) return NULL;
    net_sock_t* sock = safe_calloc(sizeof(net_sock_t), 1);
    sock->fd = fd;
    return sock;
}

// Wrap assigned local address after net_bind_handle()
static net_sock_t* net_init_localaddr(net_sock_t* sock, const net_addr_t* addr)
{
    if (sock) {
        if (addr == NULL || addr->type == NET_TYPE_IPV4) {
            struct sockaddr_in sock_addr;
            net_addrlen_t addr_len = sizeof(struct sockaddr_in);
            // Win32 getsockname may not set sin_family/sin_addr...
            net_sockaddr_from_addr(&sock_addr, addr);
            getsockname(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr(&sock->addr, &sock_addr);
#if defined(IPV6_NET_IMPL)
        } else if (addr->type == NET_TYPE_IPV6) {
            struct sockaddr_in6 sock_addr;
            net_addrlen_t addr_len = sizeof(struct sockaddr_in6);
            net_sockaddr6_from_addr(&sock_addr, addr);
            getsockname(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr6(&sock->addr, &sock_addr);
#endif
        }
    }
    return sock;
}

// Public socket API

net_sock_t* net_tcp_listen(const net_addr_t* addr)
{
    net_handle_t fd = net_create_handle(SOCK_STREAM, addr, false);
    if (fd == NET_HANDLE_INVALID) return NULL;
#if defined(SOL_SOCKET) && defined(SO_REUSEADDR)
    // Prevent bind errors due to TIME_WAIT
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse));
#endif
    if (!net_bind_handle(fd, addr) || listen(fd, SOMAXCONN)) {
        net_close_handle(fd);
        return NULL;
    }

    return net_init_localaddr(net_wrap_handle(fd), addr);
}

net_sock_t* net_tcp_accept(net_sock_t* listener)
{
    net_sock_t* sock = NULL;
    if (listener == NULL) return NULL;
    if (listener->addr.type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in);
        sock = net_wrap_handle(net_accept_ex(listener->fd, &sock_addr, &addr_len));
        if (sock) net_addr_from_sockaddr(&sock->addr, &sock_addr);
#if defined(IPV6_NET_IMPL)
    } else if (listener->addr.type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in6);
        sock = net_wrap_handle(net_accept_ex(listener->fd, &sock_addr, &addr_len));
        if (sock) net_addr_from_sockaddr6(&sock->addr, &sock_addr);
#endif
    }
    return sock;
}

net_sock_t* net_tcp_connect(const net_addr_t* dst, const net_addr_t* src, bool block)
{
    if (dst == NULL) return NULL;
    // Create a nonblocking socket if needed
    net_handle_t fd = net_create_handle(SOCK_STREAM, dst, !block);
    if (fd == NET_HANDLE_INVALID) return NULL;
    // Bind to local address if needed
    if (src) {
#if defined(SOL_SOCKET) && defined(IP_BIND_ADDRESS_NO_PORT)
        if (src->port == 0) {
            // Prevent bind errors due to ephemeral port exhaustion
            // Kernel now knows we won't listen() and allows local 2-tuple reuse
            int noport = 1;
            setsockopt(fd, SOL_SOCKET, IP_BIND_ADDRESS_NO_PORT, (const void*)&noport, sizeof(noport));
        }
#endif
#if defined(SOL_SOCKET) && defined(SO_REUSEADDR)
        if (src->port) {
            // Allow connecting to different destinations from a single local port
            int reuse = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse));
        }
#endif
        if (!net_bind_handle(fd, src)) {
            net_close_handle(fd);
            return NULL;
        }
    }

    if (!net_connect_handle(fd, dst)) {
        net_close_handle(fd);
        return NULL;
    }

    net_sock_t* sock = net_wrap_handle(fd);
    if (sock) sock->addr = *dst;

    return sock;
}

bool net_tcp_sockpair(net_sock_t* pair[2])
{
    net_sock_t* listener = net_tcp_listen(NET_IPV4_LOCAL);
    pair[0] = net_tcp_connect(net_sock_addr(listener), NULL, false);
    pair[1] = net_tcp_accept(listener);
    net_sock_close(listener);
    if (!net_tcp_status(pair[0]) || !net_tcp_status(pair[1])) {
        net_sock_close(pair[0]);
        net_sock_close(pair[1]);
        return false;
    }
    return true;
}

bool net_tcp_status(net_sock_t* sock)
{
    if (sock == NULL) return false;
    if (sock->addr.type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in);
        return getpeername(sock->fd, (struct sockaddr*)&sock_addr, &addr_len) == 0;
#if defined(IPV6_NET_IMPL)
    } else if (sock->addr.type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in6);
        return getpeername(sock->fd, (struct sockaddr*)&sock_addr, &addr_len) == 0;
#endif
    }
    return false;
}

bool net_tcp_shutdown(net_sock_t* sock)
{
    return sock && shutdown(sock->fd, 1) == 0;
}

size_t net_tcp_send(net_sock_t* sock, const void* buffer, size_t size)
{
    int ret = sock ? send(sock->fd, buffer, size, 0) : 0;
    return ret > 0 ? ret : 0;
}

size_t net_tcp_recv(net_sock_t* sock, void* buffer, size_t size)
{
    int ret = sock ? recv(sock->fd, buffer, size, 0) : 0;
    return ret > 0 ? ret : 0;
}

net_sock_t* net_udp_bind(const net_addr_t* addr)
{
    net_handle_t fd = net_create_handle(SOCK_DGRAM, addr, false);
    if (fd == NET_HANDLE_INVALID) return NULL;

    if (!net_bind_handle(fd, addr)) {
        net_close_handle(fd);
        return NULL;
    }

    return net_init_localaddr(net_wrap_handle(fd), addr);
}

size_t net_udp_send(net_sock_t* sock, const void* buffer, size_t size, const net_addr_t* addr)
{
    int ret = 0;
    if (sock == NULL) return 0;
    if (sock->addr.type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr;
        net_sockaddr_from_addr(&sock_addr, addr);
        ret = sendto(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
#if defined(IPV6_NET_IMPL)
    } else if (sock->addr.type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr;
        net_sockaddr6_from_addr(&sock_addr, addr);
        ret = sendto(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
#endif
    }
    return ret > 0 ? ret : 0;
}

size_t net_udp_recv(net_sock_t* sock, void* buffer, size_t size, net_addr_t* addr)
{
    int ret = 0;
    if (sock == NULL) return 0;
    if (sock->addr.type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in);
        ret = recvfrom(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, &addr_len);
        net_addr_from_sockaddr(addr, &sock_addr);
#if defined(IPV6_NET_IMPL)
    } else if (sock->addr.type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr = {0};
        net_addrlen_t addr_len = sizeof(struct sockaddr_in6);
        ret = recvfrom(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, &addr_len);
        net_addr_from_sockaddr6(addr, &sock_addr);
#endif
    }
    return ret > 0 ? ret : 0;
}

// Generic socket operations

const net_addr_t* net_sock_addr(net_sock_t* sock)
{
    return sock ? &sock->addr : NET_IPV4_ANY;
}

uint16_t net_sock_port(net_sock_t* sock)
{
    return sock ? sock->addr.port : 0;
}

bool net_sock_set_blocking(net_sock_t* sock, bool block)
{
    return sock && net_handle_set_blocking(sock->fd, block);
}

void net_sock_close(net_sock_t* sock)
{
    if (sock == NULL) return;
#if defined(SELECT_NET_IMPL)
    vector_foreach_back(sock->watchers, i) {
        net_poll_remove(vector_at(sock->watchers, i), sock);
    }
    vector_free(sock->watchers);
#endif
    net_close_handle(sock->fd);
#if defined(WSA_NET_IMPL)
    if (sock->event != NULL && sock->event != WSA_INVALID_EVENT) {
        UnregisterWaitEx(sock->wait, WSA_INVALID_EVENT);
        WSACloseEvent(sock->event);
        vector_foreach_back(sock->watchers, i) {
            net_poll_remove(vector_at(sock->watchers, i).poll, sock);
        }
        vector_free(sock->watchers);
    }
#endif
    free(sock);
}

// Event polling

net_poll_t* net_poll_create()
{
    net_init();
    net_poll_t* poll = calloc(sizeof(net_poll_t), 1);
    if (poll == NULL) return NULL;
#if defined(EPOLL_NET_IMPL)
    poll->fd = epoll_create(16);
    if (poll->fd < 0) {
        free(poll);
        return NULL;
    }
    net_handle_set_cloexec(poll->fd);
#elif defined(KQUEUE_NET_IMPL)
    poll->fd = kqueue();
    if (poll->fd < 0) {
        free(poll);
        return NULL;
    }
    net_handle_set_cloexec(poll->fd);
#elif defined(WSA_NET_IMPL)
    hashmap_init(&poll->events, 16);
    poll->cond = condvar_create();
#else
    vector_init(poll->events);
    FD_ZERO(&poll->r_set);
    FD_ZERO(&poll->w_set);
    poll->max_fd = 1;
#endif
    return poll;
}

#if defined(WSA_NET_IMPL)
static void CALLBACK wsa_callback(void* param, BOOLEAN timeout)
{
    if (timeout) return;
    net_sock_t* sock = (net_sock_t*)param;

    WSANETWORKEVENTS ev;
    if (WSAEnumNetworkEvents(sock->fd, sock->event, &ev) == 0) {
        vector_foreach(sock->watchers, i) {
            net_watch_t* watch = &vector_at(sock->watchers, i);
            net_poll_t* poll = watch->poll;
            uint32_t flags = watch->flags & ev.lNetworkEvents;
            if (flags) {
                spin_lock(&poll->lock);
                size_t tmp = hashmap_get(&poll->events, (size_t)watch->data);
                hashmap_put(&poll->events, (size_t)watch->data, tmp | flags);
                spin_unlock(&poll->lock);
                condvar_wake(poll->cond);
            }
        }
    }
}
#endif

bool net_poll_add(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    if (poll == NULL || sock == NULL) return false;
    bool poll_wr = !!(event->flags & NET_POLL_SEND);
#if defined(EPOLL_NET_IMPL)
    struct epoll_event ev = {
        .events = EPOLLIN | (poll_wr ? EPOLLOUT : 0),
        .data.ptr = event->data,
    };
    return epoll_ctl(poll->fd, EPOLL_CTL_ADD, sock->fd, &ev) == 0;
#elif defined(KQUEUE_NET_IMPL)
    struct kevent ev[2];
    EV_SET(&ev[0], sock->fd, EVFILT_READ, EV_ADD, 0, 0, event->data);
    EV_SET(&ev[1], sock->fd, EVFILT_WRITE, poll_wr ? EV_ADD : EV_DELETE, 0, 0, event->data);
    return kevent(poll->fd, ev, 2, NULL, 0, NULL) != -1 || errno == ENOENT;
#elif defined(WSA_NET_IMPL)
    net_watch_t watch = {
        .poll = poll,
        .data = event->data,
        .flags = FD_READ | FD_ACCEPT | FD_CLOSE | (poll_wr ? (FD_WRITE | FD_CONNECT) : 0),
    };
    if (sock->event == NULL || sock->event == WSA_INVALID_EVENT) {
        sock->event = WSACreateEvent();
        // We cannot attach multiple events to socket, thus it's slightly inefficient
        WSAEventSelect(sock->fd, sock->event, FD_READ | FD_ACCEPT | FD_CLOSE | FD_WRITE | FD_CONNECT);
        RegisterWaitForSingleObject(&sock->wait, sock->event, wsa_callback, sock, INFINITE, 0);
    }
    vector_foreach(sock->watchers, i) {
        if (vector_at(sock->watchers, i).poll == poll) return false;
    }
    vector_push_back(sock->watchers, watch);
    return true;
#else
    net_monitor_t monitor = {
        .sock = sock,
        .data = event->data,
        .flags = event->flags | NET_POLL_RECV,
    };
    spin_lock(&poll->lock);
    if (FD_ISSET(sock->fd, &poll->r_set) || FD_ISSET(sock->fd, &poll->w_set)) {
        // Socket already monitored
        spin_unlock(&poll->lock);
        return false;
    }
#ifdef _WIN32
    if (vector_size(poll->events) >= FD_SETSIZE)
#else
    if (sock->fd >= FD_SETSIZE)
#endif
    {
        rvvm_warn("select(): ignoring sockets above FD_SETSIZE (%d)", (uint32_t)FD_SETSIZE);
        spin_unlock(&poll->lock);
        return false;
    }
#ifndef _WIN32
    if (poll->max_fd < sock->fd) poll->max_fd = sock->fd;
#endif
    // Monitor for requested events
    FD_SET(sock->fd, &poll->r_set);
    if (poll_wr) FD_SET(sock->fd, &poll->w_set);
    vector_push_back(poll->events, monitor);
    // Link the socket to the watcher to remove() it on close()
    vector_push_back(sock->watchers, poll);
    spin_unlock(&poll->lock);
    return true;
#endif
}

bool net_poll_mod(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    if (poll == NULL || sock == NULL) return false;
    bool poll_wr = !!(event->flags & NET_POLL_SEND);
#if defined(EPOLL_NET_IMPL)
    struct epoll_event ev = {
        .events = EPOLLIN | (poll_wr ? EPOLLOUT : 0),
        .data.ptr = event->data,
    };
    return epoll_ctl(poll->fd, EPOLL_CTL_MOD, sock->fd, &ev) == 0;
#elif defined(KQUEUE_NET_IMPL)
    struct kevent ev[2];
    EV_SET(&ev[0], sock->fd, EVFILT_READ, EV_ADD, 0, 0, event->data);
    EV_SET(&ev[1], sock->fd, EVFILT_WRITE, poll_wr ? EV_ADD : EV_DELETE, 0, 0, event->data);
    return kevent(poll->fd, ev, 2, NULL, 0, NULL) != -1 || errno == ENOENT;
#elif defined(WSA_NET_IMPL)
    vector_foreach(sock->watchers, i) {
        net_watch_t* watch = &vector_at(sock->watchers, i);
        if (watch->poll == poll) {
            watch->data = event->data;
            watch->flags = FD_READ | FD_ACCEPT | FD_CLOSE
             | (poll_wr ? (FD_WRITE | FD_CONNECT) : 0);
            return true;
        }
    }
    return false;
#else
    spin_lock(&poll->lock);
    vector_foreach(poll->events, i) {
        net_monitor_t* monitor = &vector_at(poll->events, i);
        if (monitor->sock == sock) {
            monitor->data = event->data;
            monitor->flags = NET_POLL_RECV | (poll_wr ? NET_POLL_SEND : 0);
            if (poll_wr) {
                FD_SET(sock->fd, &poll->w_set);
            } else {
                FD_CLR(sock->fd, &poll->w_set);
            }
            spin_unlock(&poll->lock);
            return true;
        }
    }
    spin_unlock(&poll->lock);
    return false;
#endif
}

bool net_poll_remove(net_poll_t* poll, net_sock_t* sock)
{
    if (poll == NULL || sock == NULL) return false;
#if defined(EPOLL_NET_IMPL)
    struct epoll_event ev = {0};
    return epoll_ctl(poll->fd, EPOLL_CTL_DEL, sock->fd, &ev) == 0;
#elif defined(KQUEUE_NET_IMPL)
    struct kevent ev[2];
    EV_SET(&ev[0], sock->fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], sock->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    return kevent(poll->fd, ev, 2, NULL, 0, NULL) != -1 || errno == ENOENT;
#elif defined(WSA_NET_IMPL)
    vector_foreach(sock->watchers, i) {
        net_watch_t* watch = &vector_at(sock->watchers, i);
        if (watch->poll == poll) {
            spin_lock(&watch->poll->lock);
            hashmap_remove(&watch->poll->events, (size_t)watch->data);
            spin_unlock(&watch->poll->lock);
            vector_erase(sock->watchers, i);
            return true;
        }
    }
    return false;
#else
    spin_lock(&poll->lock);
    vector_foreach(poll->events, i) {
        if (vector_at(poll->events, i).sock == sock) {
            vector_erase(poll->events, i);
            FD_CLR(sock->fd, &poll->r_set);
            FD_CLR(sock->fd, &poll->w_set);
            // Skip this socket in events buffer
            if (poll->consumed > i) poll->consumed--;
            // Unlink watcher from the socket
            vector_foreach(sock->watchers, j) {
                if (vector_at(sock->watchers, j) == poll) {
                    vector_erase(sock->watchers, j);
                    spin_unlock(&poll->lock);
                    return true;
                }
            }
            rvvm_warn("Corrupted socket watcher list!");
            break;
        }
    }
    spin_unlock(&poll->lock);
    return false;
#endif
}

#define NET_POLL_MAX_EVENTS 64

size_t net_poll_wait(net_poll_t* poll, net_event_t* events, size_t size, uint32_t wait_ms)
{
    if (poll == NULL || size == 0) return 0;
#if defined(EPOLL_NET_IMPL)
    struct epoll_event ev[NET_POLL_MAX_EVENTS];
    if (size > NET_POLL_MAX_EVENTS) size = NET_POLL_MAX_EVENTS;
    int ret = epoll_wait(poll->fd, ev, size, wait_ms);
    if (ret < 0) ret = 0;
    for (int i=0; i<ret; ++i) {
        events[i].data = ev[i].data.ptr;
        events[i].flags = ((ev[i].events & ~EPOLLOUT) ? NET_POLL_RECV : 0)
                        | ((ev[i].events & EPOLLOUT) ? NET_POLL_SEND : 0);
    }
#elif defined(KQUEUE_NET_IMPL)
    size_t ret = 0;
    struct kevent ev[NET_POLL_MAX_EVENTS];
    struct timespec ts = {
        .tv_sec = wait_ms / 1000,
        .tv_nsec = (wait_ms % 1000) * 1000000,
    };
    if (size > NET_POLL_MAX_EVENTS) size = NET_POLL_MAX_EVENTS;
    int cnt = kevent(poll->fd, NULL, 0, ev, size, (wait_ms == NET_POLL_INF) ? NULL : &ts);
    for (int i=0; i<cnt; ++i) if (ev[i].filter == EVFILT_READ) {
        events[ret].data = (void*)ev[i].udata;
        events[ret++].flags = NET_POLL_RECV;
    }
    // Coalesce NET_POLL_SEND flags onto associated event entry
    for (int i=0; i<cnt; ++i) if (ev[i].filter == EVFILT_WRITE) {
        bool coalesce = false;
        for (size_t j=0; j<ret; ++j) if (events[j].data == (void*)ev[i].udata) {
            events[j].flags |= NET_POLL_SEND;
            coalesce = true;
            break;
        }
        if (!coalesce) {
            events[ret].data = (void*)ev[i].udata;
            events[ret++].flags = NET_POLL_SEND;
        }
    }
#elif defined(WSA_NET_IMPL)
    size_t ret = 0;
    do {
        // Try consuming existing events
        spin_lock(&poll->lock);
        hashmap_foreach(&poll->events, k, v) {
            events[ret].data = (void*)k;
            events[ret].flags = v;
            if (++ret >= size) break;
        }
        if (ret) {
            // Clear consumed event flags
            for (size_t i=0; i<ret; ++i) {
                uint32_t flags = events[i].flags;
                if (flags & (FD_READ | FD_ACCEPT)) {
                    flags &= ~(FD_READ | FD_ACCEPT);
                    events[i].flags = NET_POLL_RECV;
                } else if (flags & (FD_WRITE | FD_CONNECT)) {
                    flags &= ~(FD_WRITE | FD_CONNECT);
                    events[i].flags = NET_POLL_SEND;
                } else if (flags & FD_CLOSE) {
                    flags = FD_CLOSE;
                    events[i].flags = NET_POLL_RECV;
                } else {
                    rvvm_warn("Unknown watch flags %x in net_poll_wait()!", flags);
                    flags = 0;
                    events[i].flags = 0;
                }
                if (flags) {
                    hashmap_put(&poll->events, (size_t)events[i].data, flags);
                } else {
                    hashmap_remove(&poll->events, (size_t)events[i].data);
                }
            }
        }
        spin_unlock(&poll->lock);
        // If no events are available, wait for a condvar signal
        if (ret == 0 && wait_ms) {
            if (!condvar_wait(poll->cond, wait_ms)) {
                wait_ms = 0;
            }
        } else break;
    } while (ret == 0);
#else
    size_t ret = 0;

    spin_lock(&poll->lock);
    do {
        bool has_events = poll->consumed;
        if (!has_events) {
            // No available buffered events to consume
            // Wait for small intervals, allowing to modify polled events
            int nfds = poll->max_fd + 1;
            struct timeval tv = {
                .tv_usec = (wait_ms < 10) ? wait_ms : 10,
            };
            if (wait_ms != NET_POLL_INF) wait_ms -= tv.tv_usec;
            tv.tv_usec *= 1000;

            poll->r_ready = poll->r_set;
            poll->w_ready = poll->w_set;
            spin_unlock(&poll->lock);
            has_events = select(nfds, &poll->r_ready, &poll->w_ready, NULL, &tv) > 0;
            spin_lock(&poll->lock);
        }

        // Loop over buffered socket state
        if (has_events) for (size_t i=poll->consumed; i<vector_size(poll->events); ++i) {
            net_monitor_t* monitor = &vector_at(poll->events, i);
            uint32_t flags = 0;
            if (monitor->flags & NET_POLL_RECV) {
                if (FD_ISSET(monitor->sock->fd, &poll->r_ready)) flags |= NET_POLL_RECV;
            }
            if (monitor->flags & NET_POLL_SEND) {
                if (FD_ISSET(monitor->sock->fd, &poll->w_ready)) flags |= NET_POLL_SEND;
            }
            if (flags) {
                events[ret].data = monitor->data;
                events[ret].flags = flags;
                if (++ret >= size) {
                    // We filled caller event buffer, leave trailing buffered events
                    poll->consumed = i + 1;
                    spin_unlock(&poll->lock);
                    return ret;
                }
            }
        }
        // All events consumed, call select() next time
        poll->consumed = 0;
    } while (wait_ms && ret == 0);
    spin_unlock(&poll->lock);
#endif
    return ret;
}

void net_poll_close(net_poll_t* poll)
{
    if (poll == NULL) return;
#if defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL)
    net_close_handle(poll->fd);
#elif defined(WSA_NET_IMPL)
    hashmap_destroy(&poll->events);
    condvar_free(poll->cond);
#else
    // Unlink watcher from related sockets
    vector_foreach(poll->events, i) {
        net_sock_t* sock = vector_at(poll->events, i).sock;
        vector_foreach(sock->watchers, j) {
            if (vector_at(sock->watchers, j) == poll) {
                vector_erase(sock->watchers, j);
                break;
            }
        }
    }
    vector_free(poll->events);
#endif
    free(poll);
}
