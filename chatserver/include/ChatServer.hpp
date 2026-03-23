#pragma once

#include "../mymuduo/TcpServer.h"
#include "../mymuduo/EventLoop.h"
#include <functional>
#include <string>
#include <unordered_map>

using namespace std;

class ChatServer {
public:
    ChatServer(EventLoop* loop, const InetAddress& listenAddr, const string& nameArg);
    ~ChatServer();

    void start();

    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buffer, Timestamp time);

private:
    TcpServer _server;
    EventLoop* _loop;
};
