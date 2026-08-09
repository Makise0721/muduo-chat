#include "ChatServer.hpp"
#include "ChatService.hpp"
#include <iostream>
#include <cstring>

ChatServer::ChatServer(EventLoop* loop, const InetAddress& listenAddr, const string& nameArg,
                       ProtocolCodec codec)
    : _server(loop, listenAddr, nameArg), _loop(loop), _codec(codec) {
    
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, std::placeholders::_1));
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, 
                                         std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

ChatServer::~ChatServer() {
}

void ChatServer::start() {
    _server.start();
}

void ChatServer::stopAccept() {
    _server.stopAccept();
}

int ChatServer::connectionCount() const {
    return _server.connectionCount();
}

void ChatServer::forceCloseAllConnections() {
    _server.forceCloseAllConnections();
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        std::cout << "Connection established: " << conn->peerAddress().toIpPort() << std::endl;
        if (_codec == ProtocolCodec::BinaryFrame) {
            conn->setOutputEncoder([this](const std::string &message, Buffer *output) {
                _frameCodec.encode(message, output);
            });
        }
    } else {
        std::cout << "Connection closed: " << conn->peerAddress().toIpPort() << std::endl;
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr& conn, Buffer* buffer, Timestamp time) {
    std::string message;
    if (_codec == ProtocolCodec::BinaryFrame) {
        while (true) {
            CodecResult r = _frameCodec.decode(buffer, &message);
            if (r == CodecResult::Message) {
                ChatService::instance()->handler(conn, message, time);
            } else if (r == CodecResult::ProtocolError) {
                conn->shutdown();
                return;
            } else {
                return;
            }
        }
    } else {
        while (_lineCodec.decode(buffer, &message)) {
            ChatService::instance()->handler(conn, message, time);
        }
    }
}
