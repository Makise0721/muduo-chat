#pragma once

#include "../mymuduo/TcpServer.h"
#include "../mymuduo/EventLoop.h"
#include "../mymuduo/LegacyJsonLineCodec.h"
#include "../mymuduo/BinaryFrameCodec.h"
#include <functional>
#include <string>
#include <unordered_map>

using namespace std;

enum class ProtocolCodec
{
    LegacyLine,
    BinaryFrame,
};

class ChatServer {
public:
    ChatServer(EventLoop* loop, const InetAddress& listenAddr, const string& nameArg,
               ProtocolCodec codec);
    ~ChatServer();

    void start();
    void stopAccept();
    int connectionCount() const;
    void forceCloseAllConnections();
    // P2-08：多 Reactor——I/O 线程数（在 start() 前调用）。
    void setThreadNum(int numThreads);

    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buffer, Timestamp time);

private:
    TcpServer _server;
    EventLoop* _loop;
    ProtocolCodec _codec;
    LegacyJsonLineCodec _lineCodec;
    BinaryFrameCodec _frameCodec;
};
