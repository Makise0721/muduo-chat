#include "Socket.h"
#include "Logger.h"

#include <unistd.h>
#include <strings.h>
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
        LOG_FATAL("Socket::bindAddress failed");
    }
}

void Socket::listen()
{
    if (0 != ::listen(sockfd_, SOMAXCONN))
    {
        LOG_FATAL("Socket::listen failed");
    }
}

int Socket::accept(InetAddress *peeraddr)
{
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int connfd = ::accept(sockfd_, (sockaddr *)&addr, &addrlen);
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