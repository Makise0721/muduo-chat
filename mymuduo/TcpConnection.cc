#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("TcpConnection::TcpConnection() - SubLoop is null!");
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &name,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop)),
      name_(name),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024)
{
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::TcpConnection() - new connection [%s] from %s to %s",
             name_.c_str(),
             peerAddr_.toIpPort().c_str(),
             localAddr_.toIpPort().c_str());
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::~TcpConnection() - connection [%s] from %s to %s is down. "
             "LocalAddr[%s], PeerAddr[%s]",
             name_.c_str(),
             peerAddr_.toIpPort().c_str(),
             localAddr_.toIpPort().c_str(),
             localAddr_.toIpPort().c_str(),
             peerAddr_.toIpPort().c_str());
}

TcpConnection::SendResult TcpConnection::send(std::string message)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            return sendInLoop(std::move(message));
        }
        else
        {
            if (message.size() > limits_.hardLimitBytes)
            {
                return SendResult::TooLarge;
            }
            using SendInLoop = SendResult (TcpConnection::*)(std::string);
            loop_->runInLoop(std::bind(static_cast<SendInLoop>(&TcpConnection::sendInLoop),
                                       shared_from_this(), std::move(message)));
            return SendResult::Accepted;
        }
    }
    else
    {
        LOG_ERROR("TcpConnection::send() - Connection [%s] is down, can not send message", name_.c_str());
        return SendResult::Closed;
    }
}

TcpConnection::SendResult TcpConnection::sendInLoop(std::string message)
{
    if (encoder_)
    {
        Buffer encoded;
        encoder_(message, &encoded);
        return sendInLoop(encoded.peek(), encoded.readableBytes());
    }
    return sendInLoop(message.data(), message.size());
}

TcpConnection::SendResult TcpConnection::sendInLoop(const void *message, size_t len)
{
    if (state_ == kDisconnected)
    {
        LOG_ERROR("TcpConnection::sendInLoop() - Connection [%s] is disconnected, give up writing", name_.c_str());
        return SendResult::Closed;
    }

    if (len > limits_.hardLimitBytes)
    {
        return SendResult::TooLarge;
    }

    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), message, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)
            {
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }
        else
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop() - error %d", errno);
                if (errno == EPIPE || errno == ECONNRESET)
                {
                    faultError = true;
                }
            }
        }
    }

    if (!faultError && remaining > 0)
    {
        if (outputBuffer_.readableBytes() >= limits_.hardLimitBytes)
        {
            return SendResult::Backpressured;
        }
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_)
        {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append(static_cast<const char *>(message) + nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting();
        }
        checkPause();
    }
    return SendResult::Accepted;
}

void TcpConnection::checkPause()
{
    if (outputBuffer_.readableBytes() >= limits_.pauseReadBytes && reading_)
    {
        channel_->disableReading();
        reading_ = false;
        startStallTimer();
    }
}

void TcpConnection::startStallTimer()
{
    if (!stallActive_)
    {
        TcpConnectionPtr self(shared_from_this());
        stallTimerId_ = loop_->runAfter(limits_.stallTimeout.count(),
                                        [self] { self->forceClose(); });
        stallActive_ = true;
    }
}

void TcpConnection::cancelStallTimer()
{
    if (stallActive_)
    {
        loop_->cancel(stallTimerId_);
        stallActive_ = false;
        stallTimerId_ = TimerId();
    }
}

void TcpConnection::forceClose()
{
    cancelStallTimer();
    if (state_ == kConnected || state_ == kDisconnecting)
    {
        setState(kDisconnected);
        channel_->disableAll();
        TcpConnectionPtr guardThis(shared_from_this());
        connectionCallback_(guardThis);
        if (closeCallback_)
        {
            closeCallback_(guardThis);
        }
    }
}
void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, shared_from_this()));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting())
    {
        socket_->shutdownWrite();
    }
}

void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();

    connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll();
        connectionCallback_(shared_from_this());
    }
    cancelStallTimer();
    channel_->remove();
}

void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0)
    {
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0)
    {
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead() - error %d", errno);
        handleError();
    }
}

void TcpConnection::handleWrite()
{
    if (channel_->isWriting())
    {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting();
                if (writeCompleteCallback_)
                {
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this()));
                }
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }
            if (!reading_ && outputBuffer_.readableBytes() <= limits_.resumeReadBytes)
            {
                channel_->enableReading();
                reading_ = true;
                cancelStallTimer();
            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite() - error %d", savedErrno);
        }
    }
    else
    {
        LOG_ERROR("TcpConnection::handleWrite() - Connection [%s] is down, no more writing", name_.c_str());
    }
}

void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose() - connection [%s] from %s to %s is closed",
             name_.c_str(),
             peerAddr_.toIpPort().c_str(),
             localAddr_.toIpPort().c_str());
    cancelStallTimer();
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    connectionCallback_(guardThis);
    closeCallback_(guardThis);
}

void TcpConnection::handleError()
{
    int optval, err = 0;
    socklen_t optlen = sizeof optval;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError() - SO_ERROR: %d ", err);
}