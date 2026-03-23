#include "ChatServer.hpp"
#include "ChatService.hpp"
#include <iostream>
#include <cstring>

ChatServer::ChatServer(EventLoop* loop, const InetAddress& listenAddr, const string& nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop) {
    
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, std::placeholders::_1));
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, 
                                         std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

ChatServer::~ChatServer() {
}

void ChatServer::start() {
    _server.start();
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        std::cout << "Connection established: " << conn->peerAddress().toIpPort() << std::endl;
    } else {
        std::cout << "Connection closed: " << conn->peerAddress().toIpPort() << std::endl;
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr& conn, Buffer* buffer, Timestamp time) {
    // 协议：一行一个 JSON（以 '\n' 作为消息结束符）
    while (true) {
        const char* data = buffer->peek();
        size_t len = buffer->readableBytes();
        if (len == 0) {
            return;
        }

        const void* nl = memchr(data, '\n', len);
        if (!nl) {
            // 当前没有完整一条消息（还缺 '\n'），等待下一次读事件
            return;
        }

        size_t lineLen = static_cast<const char*>(nl) - data;
        std::string line(data, lineLen);

        // 消耗掉这一行（包含 '\n'）
        buffer->retrieve(lineLen + 1);

        // 兼容 Windows 换行：\r\n
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        ChatService::instance()->handler(conn, line, time);
    }
}
