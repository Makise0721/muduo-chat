#pragma once

#include <chrono>
#include <memory>

#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"

class EventLoop;
class InetAddress;

class Acceptor : noncopyable
{
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &peeraddr)>;

    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();

    void listen();

    void stopListen()
    {
        listening_ = false;
        acceptChannel_.disableAll();
    }
    
    void setNewConnectionCallback(const NewConnectionCallback &cb)
    {
        newConnectionCallback_ = cb;
    }

    void setAcceptRateLimit(double ratePerSecond, int burst)
    {
        acceptRatePerSecond_ = ratePerSecond;
        acceptBurst_ = burst;
        tokens_ = burst;
        lastRefill_ = std::chrono::steady_clock::now();
    }

    bool listening() const { return listening_; }

    int listenFd() const { return acceptSocket_.fd(); }

    int acceptErrorCount() const { return acceptErrors_; }

    int rateRejectedCount() const { return rateRejected_; }
private:
    void handleRead();
    void retryAccept();
    bool tryTakeToken();

    EventLoop *loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    bool acceptPaused_ = false;
    int acceptErrors_ = 0;
    int rateRejected_ = 0;
    int idleFd_ = -1;
    double acceptRatePerSecond_ = 0;
    int acceptBurst_ = 0;
    double tokens_ = 0;
    std::chrono::steady_clock::time_point lastRefill_;
};