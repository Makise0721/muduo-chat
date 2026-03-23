#include "ChatServer.hpp"
#include "ChatService.hpp"
#include <iostream>

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
    string buf = buffer->retrieveAllAsString();
    buffer->retrieveAll();    
    
    ChatService::instance()->handler(conn, buf, time);
}
