#include "ChatServer.hpp"
#include "ChatService.hpp"
#include "db/ConnectionPool.hpp"
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>

namespace {

int gSignalFds[2];
static bool gShuttingDown = false;

void signalHandler(int) {
    char c = 1;
    ssize_t n = ::write(gSignalFds[1], &c, 1);
    (void)n;
}

void beginShutdown(EventLoop* loop, ChatServer* v1, ChatServer* v2) {
    if (gShuttingDown) {
        return;
    }
    gShuttingDown = true;
    std::cout << "Shutdown: stopping accept" << std::endl;
    v1->stopAccept();
    v2->stopAccept();

    std::function<void()> check;
    check = [loop, v1, v2]() {
        int pending = v1->connectionCount() + v2->connectionCount();
        if (pending == 0) {
            std::cout << "DRAINED pending=0" << std::endl;
            loop->quit();
        }
    };
    loop->runEvery(50, check);

    loop->runAfter(5000, [loop, v1, v2]() {
        int pending = v1->connectionCount() + v2->connectionCount();
        std::cout << "DRAIN_TIMEOUT pending=" << pending << std::endl;
        v1->forceCloseAllConnections();
        v2->forceCloseAllConnections();
        loop->quit();
    });
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "command invalid! example: ./ChatServer 127.0.0.1 6000" << std::endl;
        exit(-1);
    }

    char* ip = argv[1];
    uint16_t port = atoi(argv[2]);

    auto& connPool = ConnectionPool::getInstance();
    const char* dbPassword = getenv("DB_PASSWORD");
    if (!dbPassword) {
        dbPassword = "123456";
        std::cerr << "Warning: DB_PASSWORD environment variable not set, using default password '123456'" << std::endl;
    }
    connPool.init("127.0.0.1", "root", dbPassword, "chat", 3306, 5);

    EventLoop loop;
    InetAddress addr(port, ip);
    std::cout << "Server starting on " << ip << ":" << port << " (v1 newline JSON)" << std::endl;
    ChatServer server(&loop, addr, "ChatServer", ProtocolCodec::LegacyLine);

    InetAddress v2Addr(7000, ip);
    std::cout << "Server starting on " << ip << ":7000 (v2 binary)" << std::endl;
    ChatServer v2Server(&loop, v2Addr, "ChatServerV2", ProtocolCodec::BinaryFrame);

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, gSignalFds) < 0) {
        std::cerr << "socketpair failed" << std::endl;
        exit(-1);
    }
    int sigFlags = fcntl(gSignalFds[0], F_GETFL, 0);
    fcntl(gSignalFds[0], F_SETFL, sigFlags | O_NONBLOCK);
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    Channel sigChannel(&loop, gSignalFds[0]);
    sigChannel.setReadCallback([&loop, &server, &v2Server](Timestamp) {
        char buf[16];
        while (::read(gSignalFds[0], buf, sizeof buf) > 0)
        {
        }
        beginShutdown(&loop, &server, &v2Server);
    });
    sigChannel.enableReading();

    std::cout << "Server started, entering event loop" << std::endl;
    server.start();
    v2Server.start();
    loop.loop();

    std::cout << "Server exited" << std::endl;
    return 0;
}
