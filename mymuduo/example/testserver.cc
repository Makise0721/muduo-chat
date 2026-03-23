#include "../EventLoop.h"
#include "../InetAddress.h"
#include "../TcpServer.h"
#include "../TcpConnection.h"
#include "../Buffer.h"
#include "../Timestamp.h"

#include <iostream>
#include <string>


class EchoServer 
{
public:
    EchoServer(EventLoop *loop, const InetAddress &listenAddr)
        : loop_(loop),
          server_(loop, listenAddr, "EchoServer") 
    {
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&EchoServer::onMessage, this, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3));
    }

    void start() {
        server_.start();
    }

private:

   void onConnection(const TcpConnectionPtr &conn) {
        if (conn->connected()) {
            printf("EchoServer - %s connected\n", conn->peerAddress().toIpPort().c_str());
        } else {
            printf("EchoServer - %s disconnected\n", conn->peerAddress().toIpPort().c_str());
        }
    }

    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time) {
        std::string msg = buf->retrieveAllAsString();
        printf("EchoServer - received %zd bytes from %s at %s\n",
               msg.size(), conn->peerAddress().toIpPort().c_str(),
               time.toString().c_str());
        conn->send(msg);
    }

    EventLoop *loop_;
    TcpServer server_;
};

int main() {
    EventLoop loop;
    InetAddress listenAddr(8000);
    EchoServer server(&loop, listenAddr);
    server.start();
    loop.loop();
    return 0;
}