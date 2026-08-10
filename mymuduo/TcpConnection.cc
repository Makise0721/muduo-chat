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

const uint64_t TcpConnection::kReserveFailed = static_cast<uint64_t>(-1);

TcpConnection::SendOutcome TcpConnection::send(std::string message)
{
    if (state_ != kConnected)
    {
        LOG_ERROR("TcpConnection::send() - Connection [%s] is down, can not send message", name_.c_str());
        return {SendOutcome::Disposition::Closed, SendOutcome::Pressure::Normal};
    }

    const size_t frameSize = std::max<size_t>(1, codec_ != nullptr
                                                      ? codec_->encodedSize(message.size())
                                                      : message.size());
    if (frameSize > limits_.hardLimitBytes)
    {
        return {SendOutcome::Disposition::TooLarge, SendOutcome::Pressure::Normal};
    }

    const uint64_t after = tryReserve(frameSize);
    if (after == kReserveFailed)
    {
        loop_->runInLoop(std::bind(&TcpConnection::startStallTimer, shared_from_this()));
        return {SendOutcome::Disposition::WouldBlock, SendOutcome::Pressure::PauseProducer};
    }
    const bool overPause = after > limits_.pauseReadBytes;

    if (loop_->isInLoopThread())
    {
        sendInLoop(std::move(message), frameSize);
    }
    else
    {
        using SendInLoopFn = SendOutcome (TcpConnection::*)(std::string, size_t);
        loop_->runInLoop(std::bind(static_cast<SendInLoopFn>(&TcpConnection::sendInLoop),
                                   shared_from_this(),
                                   std::move(message),
                                   frameSize));
    }
    return {SendOutcome::Disposition::Accepted,
            overPause ? SendOutcome::Pressure::PauseProducer
                      : SendOutcome::Pressure::Normal};
}

uint64_t TcpConnection::tryReserve(size_t n)
{
    uint64_t cur = outstandingBytes_.load(std::memory_order_relaxed);
    for (;;)
    {
        if (cur > limits_.hardLimitBytes - n)
        {
            return kReserveFailed;
        }
        if (outstandingBytes_.compare_exchange_weak(cur, cur + n,
                                                    std::memory_order_relaxed))
        {
            return cur + n;
        }
    }
}

void TcpConnection::releaseOutstanding(uint64_t n)
{
    const uint64_t before = outstandingBytes_.fetch_sub(n, std::memory_order_relaxed);
    const uint64_t after = before - n;
    if (lowWaterArmed_ && before > limits_.resumeReadBytes &&
        after <= limits_.resumeReadBytes)
    {
        lowWaterArmed_ = false;
        if (pressureCallback_)
        {
            pressureCallback_();
        }
    }
}

void TcpConnection::resetOutstanding()
{
    outstandingBytes_.store(0, std::memory_order_relaxed);
    lowWaterArmed_ = false;
}

TcpConnection::SendOutcome TcpConnection::sendInLoop(std::string message, size_t frameSize)
{
    if (state_ == kDisconnected)
    {
        LOG_ERROR("TcpConnection::sendInLoop() - Connection [%s] is disconnected, give up writing", name_.c_str());
        releaseOutstanding(frameSize);
        return {SendOutcome::Disposition::Closed, SendOutcome::Pressure::Normal};
    }

    Buffer encoded;
    if (codec_ != nullptr)
    {
        if (codec_->encode(message, &encoded) != EncodeResult::Ok)
        {
            releaseOutstanding(frameSize);
            return {SendOutcome::Disposition::TooLarge, SendOutcome::Pressure::Normal};
        }
        return sendInLoop(encoded.peek(), encoded.readableBytes(), frameSize);
    }
    return sendInLoop(message.data(), message.size(), frameSize);
}

TcpConnection::SendOutcome TcpConnection::sendInLoop(const void *message, size_t len, size_t frameSize)
{
    if (state_ == kDisconnected)
    {
        LOG_ERROR("TcpConnection::sendInLoop() - Connection [%s] is disconnected, give up writing", name_.c_str());
        releaseOutstanding(frameSize);
        return {SendOutcome::Disposition::Closed, SendOutcome::Pressure::Normal};
    }

    if (outstandingBytes_.load(std::memory_order_relaxed) > limits_.resumeReadBytes)
    {
        lowWaterArmed_ = true;
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
            releaseOutstanding(remaining == 0 ? frameSize
                                              : static_cast<uint64_t>(nwrote));
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
    return {SendOutcome::Disposition::Accepted, SendOutcome::Pressure::Normal};
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
        std::weak_ptr<TcpConnection> weakSelf(shared_from_this());
        stallTimerId_ = loop_->runAfter(limits_.stallTimeout.count(),
                                        [weakSelf] {
                                            if (std::shared_ptr<TcpConnection> self = weakSelf.lock())
                                            {
                                                self->forceClose();
                                            }
                                        });
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
    loop_->runInLoop(
        std::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
}

void TcpConnection::forceCloseInLoop()
{
    cancelStallTimer();
    if (state_ == kConnected || state_ == kDisconnecting)
    {
        setState(kDisconnected);
        channel_->disableAll();
        resetOutstanding();
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
    resetOutstanding();
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
            releaseOutstanding(static_cast<uint64_t>(n));
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