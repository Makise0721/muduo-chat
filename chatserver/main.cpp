#include "ChatServer.hpp"
#include "ChatService.hpp"
#include "db/ConnectionPool.hpp"
#include <iostream>
#include <signal.h>
#include <unistd.h>

void resetHandler(int) {
    ChatService::instance()->reset();
    exit(0);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "command invalid! example: ./ChatServer 127.0.0.1 6000" << std::endl;
        exit(-1);
    }

    char* ip = argv[1];
    uint16_t port = atoi(argv[2]);

    signal(SIGINT, resetHandler);

    auto& connPool = ConnectionPool::getInstance();
    const char* dbPassword = getenv("DB_PASSWORD");
    if (!dbPassword) {
        dbPassword = "123456";
        std::cerr << "Warning: DB_PASSWORD environment variable not set, using default password '123456'" << std::endl;
    }
    connPool.init("127.0.0.1", "root", dbPassword, "chat", 3306, 5);

    EventLoop loop;
    InetAddress addr(port, ip);
    std::cout << "Server starting on " << ip << ":" << port << std::endl;
    ChatServer server(&loop, addr, "ChatServer");

    std::cout << "Server started, entering event loop" << std::endl;
    server.start();
    loop.loop();

    return 0;
}
