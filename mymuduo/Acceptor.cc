#include "Acceptor.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Logger.h"

static int createNonblockingSocket()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
    if (sockfd < 0)
    {
        LOG_FATAL("createNonblockingSocket() error");
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop),
      acceptSocket_(createNonblockingSocket()),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false)
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reuseport);
    acceptSocket_.bindAddress(listenAddr);
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();
    acceptChannel_.remove();
}

void Acceptor::listen()
{
    listening_ = true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}

void Acceptor::handleRead()
{
    InetAddress peeraddr(0);
    int connfd = acceptSocket_.accept(&peeraddr);
    if (connfd >= 0)
    {
        if (newConnectionCallback_)
        {
            newConnectionCallback_(connfd, peeraddr);
        }
        else
        {
            ::close(connfd);
        }
    }
    else
    {
        LOG_ERROR("in Acceptor::handleRead");
        if (errno == EMFILE)
        {
            LOG_ERROR("Acceptor::handleRead - EMFILE");
        }
    }
} 

