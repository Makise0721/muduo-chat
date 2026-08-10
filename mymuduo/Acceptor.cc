#include "Acceptor.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Logger.h"

#include <fcntl.h>
#include <unistd.h>

static int createNonblockingSocket()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
    if (sockfd < 0)
    {
        LOG_FATAL("createNonblockingSocket() error");
    }
    return sockfd;
}

static int createIdleFd()
{
    int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        LOG_ERROR("createIdleFd() failed, errno=%d", errno);
    }
    return fd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop),
      acceptSocket_(createNonblockingSocket()),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false),
      idleFd_(createIdleFd()),
      tokens_(0),
      lastRefill_(std::chrono::steady_clock::now())
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
    if (idleFd_ >= 0)
    {
        ::close(idleFd_);
    }
}

void Acceptor::listen()
{
    listening_ = true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}

bool Acceptor::tryTakeToken()
{
    if (acceptRatePerSecond_ <= 0 || acceptBurst_ <= 0)
    {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                               now - lastRefill_)
                               .count();
    lastRefill_ = now;
    tokens_ = std::min(static_cast<double>(acceptBurst_), tokens_ + elapsed * acceptRatePerSecond_);
    if (tokens_ >= 1.0)
    {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

void Acceptor::handleRead()
{
    InetAddress peeraddr(0);
    int connfd = acceptSocket_.accept(&peeraddr);
    if (connfd >= 0)
    {
        if (!tryTakeToken())
        {
            ++rateRejected_;
            LOG_ERROR("Acceptor::handleRead - accept rate limited, rejected=%d", rateRejected_);
            ::close(connfd);
            return;
        }
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
        if (errno == EMFILE || errno == ENFILE)
        {
            ++acceptErrors_;
            LOG_ERROR("Acceptor::handleRead - fd exhausted, pause accept (%d)", acceptErrors_);
            if (idleFd_ >= 0)
            {
                ::close(idleFd_);
                idleFd_ = -1;
                int drained = acceptSocket_.accept(&peeraddr);
                if (drained >= 0)
                {
                    ::close(drained);
                }
                idleFd_ = createIdleFd();
            }
            if (!acceptPaused_)
            {
                acceptPaused_ = true;
                acceptChannel_.disableReading();
                loop_->runAfter(100, std::bind(&Acceptor::retryAccept, this));
            }
        }
    }
}

void Acceptor::retryAccept()
{
    acceptPaused_ = false;
    if (listening_)
    {
        acceptChannel_.enableReading();
    }
}

