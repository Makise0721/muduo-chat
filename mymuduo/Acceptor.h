#pragma once

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

    bool listening() const { return listening_; }

    int listenFd() const { return acceptSocket_.fd(); }

    int acceptErrorCount() const { return acceptErrors_; }
private:
    void handleRead();
    void retryAccept();

    EventLoop *loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    bool acceptPaused_ = false;
    int acceptErrors_ = 0;
};