#include "Socket.h"
#include "Logger.h"

#include <unistd.h>
#include <strings.h>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

Socket::~Socket()
{
    ::close(sockfd_);
}

void Socket::bindAddress(const InetAddress &localaddr)
{
    if (0 != ::bind(sockfd_, (sockaddr *)localaddr.getSockAddrPtr(), sizeof(sockaddr_in)))
    {
        LOG_FATAL("Socket::bindAddress failed: %s", strerror(errno));
    }
}

void Socket::listen()
{
    if (0 != ::listen(sockfd_, SOMAXCONN))
    {
        LOG_FATAL("Socket::listen failed: %s", strerror(errno));
    }
}

int Socket::accept(InetAddress *peeraddr)
{
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    // accept4 显式带 SOCK_NONBLOCK：Linux 的 accept() 不继承监听 socket 的
    // O_NONBLOCK（P3-07 背压进程测试暴露：慢消费者填满内核缓冲后，阻塞式
    // write() 卡死整条 IO 线程）。
    int connfd = ::accept4(sockfd_, (sockaddr *)&addr, &addrlen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0)
    {
        peeraddr->setSockAddr(addr);
    }
    return connfd;
}

void Socket::shutdownWrite()
{
    if (::shutdown(sockfd_, SHUT_WR) < 0)
    {
        LOG_ERROR("Socket::shutdownWrite failed");
    }
}

void Socket::setTcpNoDelay(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("Socket::setTcpNoDelay failed");
    }
}

void Socket::setReuseAddr(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("Socket::setReuseAddr failed");
    }
}

void Socket::setReusePort(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("Socket::setReusePort failed");
    }
}

void Socket::setKeepAlive(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
        LOG_ERROR("Socket::setKeepAlive failed");
    }
}