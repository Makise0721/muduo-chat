#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <chrono>

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "StreamCodec.h"
#include "Timestamp.h"
#include "TimerQueue.h"
#include "Logger.h"

class Channel;
class EventLoop;
class Socket;

class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    struct SendOutcome
    {
        enum class Disposition
        {
            Accepted,
            WouldBlock,
            Closed,
            TooLarge,
        };
        enum class Pressure
        {
            Normal,
            PauseProducer,
        };
        Disposition disposition;
        Pressure pressure;
        SendOutcome() : disposition(Disposition::Closed), pressure(Pressure::Normal) {}
        SendOutcome(Disposition d, Pressure p) : disposition(d), pressure(p) {}
    };

    using PressureCallback = std::function<void()>;

    struct WriteBufferLimits
    {
        size_t pauseReadBytes = 16 * 1024 * 1024;
        size_t resumeReadBytes = 8 * 1024 * 1024;
        size_t hardLimitBytes = 64 * 1024 * 1024;
        std::chrono::milliseconds stallTimeout{5000};
    };

    TcpConnection(EventLoop *loop,
                  const std::string &name,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
    ~TcpConnection();

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }
    bool connected() const { return state_ == kConnected; }

    void setConnectionCallback(const ConnectionCallback &cb)
    {
        connectionCallback_ = cb;
    }
    void setMessageCallback(const MessageCallback &cb)
    {
        messageCallback_ = cb;
    }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb)
    {
        writeCompleteCallback_ = cb;
    }
    void setCloseCallback(const CloseCallback &cb)
    {
        closeCallback_ = cb;
    }
    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb, size_t highWaterMark)
    {
        highWaterMarkCallback_ = cb;
        highWaterMark_ = highWaterMark;
    }

    using OutputCodecPtr = const OutputCodec *;
    void setOutputCodec(const OutputCodec *codec)
    {
        codec_ = codec;
    }

    void setPressureCallback(const PressureCallback &cb)
    {
        pressureCallback_ = cb;
    }

    void setWriteBufferLimits(const WriteBufferLimits &limits)
    {
        // hardLimit may be at or below pauseReadBytes (hard-cap-only mode);
        // resumeReadBytes must stay below both.
        if (limits.resumeReadBytes < limits.pauseReadBytes &&
            limits.resumeReadBytes < limits.hardLimitBytes)
        {
            limits_ = limits;
        }
        else
        {
            LOG_ERROR("TcpConnection::setWriteBufferLimits() - invalid limits, ignored");
        }
    }

    SendOutcome send(std::string message);
    void shutdown();
    void forceClose();

    void connectEstablished();
    void connectDestroyed();

    void setState(int s) { state_ = s; }
private:
    enum StateE
    {
        kDisconnected,
        kConnecting,
        kConnected,
        kDisconnecting
    };

    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();

    void forceCloseInLoop();
    SendOutcome sendInLoop(std::string message, size_t frameSize);
    SendOutcome sendInLoop(const void *message, size_t len, size_t frameSize);
    void shutdownInLoop();
    void checkPause();
    void startStallTimer();
    void cancelStallTimer();
    uint64_t tryReserve(size_t n);
    void releaseOutstanding(uint64_t n);
    void resetOutstanding();

    static const uint64_t kReserveFailed;

    EventLoop *loop_;
    const std::string name_;
    std::atomic_int state_;
    bool reading_;

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;

    size_t highWaterMark_;

    const OutputCodec *codec_ = nullptr;
    PressureCallback pressureCallback_;
    WriteBufferLimits limits_;
    TimerId stallTimerId_;
    bool stallActive_ = false;
    std::atomic<uint64_t> outstandingBytes_{0};
    bool lowWaterArmed_ = false;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
};