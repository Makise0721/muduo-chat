#include "app/RedisConn.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

int64_t monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t remainingMs(int64_t deadline)
{
    int64_t left = deadline - monotonicMs();
    return left > 0 ? left : 0;
}

}  // namespace

RedisConn::~RedisConn()
{
    close();
}

void RedisConn::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RedisConn::connect(const std::string& host, int port, int db, int64_t timeoutMs)
{
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        close();
        return false;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close();
        return false;
    }
    int rc = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        close();
        return false;
    }
    if (rc != 0) {
        const int64_t deadline = monotonicMs() + timeoutMs;
        for (;;) {
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = ::poll(&pfd, 1, static_cast<int>(remainingMs(deadline)));
            if (pr <= 0) {
                close();
                return false;
            }
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
                close();
                return false;
            }
            break;
        }
    }
    if (db > 0) {
        Reply r = command({"SELECT", std::to_string(db)}, timeoutMs);
        if (r.isError()) {
            close();
            return false;
        }
    }
    return true;
}

bool RedisConn::sendAll(const std::string& data, int64_t deadline)
{
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = ::poll(&pfd, 1, static_cast<int>(remainingMs(deadline)));
            if (pr <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool RedisConn::recvByte(char& c, int64_t deadline)
{
    for (;;) {
        ssize_t n = ::recv(fd_, &c, 1, 0);
        if (n == 1) {
            return true;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = ::poll(&pfd, 1, static_cast<int>(remainingMs(deadline)));
            if (pr <= 0) {
                return false;
            }
            continue;
        }
        return false;  // EOF 或真实错误
    }
}

bool RedisConn::recvLine(std::string& line, int64_t deadline)
{
    line.clear();
    char c;
    for (;;) {
        if (!recvByte(c, deadline)) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
}

bool RedisConn::recvExact(std::string& out, size_t n, int64_t deadline)
{
    out.resize(n);
    size_t off = 0;
    while (off < n) {
        ssize_t got = ::recv(fd_, &out[off], n - off, 0);
        if (got > 0) {
            off += static_cast<size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = ::poll(&pfd, 1, static_cast<int>(remainingMs(deadline)));
            if (pr <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

RedisConn::Reply RedisConn::parseReply(int64_t deadline)
{
    Reply r;
    char t;
    if (!recvByte(t, deadline)) {
        r.type = Reply::Type::Error;
        r.str = "read failed";
        return r;
    }
    std::string line;
    switch (t) {
    case '+':
        if (!recvLine(line, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        r.type = Reply::Type::Simple;
        r.str = line;
        return r;
    case '-':
        if (!recvLine(line, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        r.type = Reply::Type::Error;
        r.str = line;
        return r;
    case ':':
        if (!recvLine(line, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        r.type = Reply::Type::Integer;
        r.integer = std::strtoll(line.c_str(), nullptr, 10);
        return r;
    case '$': {
        if (!recvLine(line, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        long long len = std::strtoll(line.c_str(), nullptr, 10);
        if (len < 0) {
            r.type = Reply::Type::Nil;
            return r;
        }
        std::string data;
        if (!recvExact(data, static_cast<size_t>(len), deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        char cr = 0;
        char lf = 0;
        if (!recvByte(cr, deadline) || !recvByte(lf, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        r.type = Reply::Type::Bulk;
        r.str = std::move(data);
        return r;
    }
    case '*': {
        if (!recvLine(line, deadline)) {
            r.type = Reply::Type::Error;
            r.str = "read failed";
            return r;
        }
        long long n = std::strtoll(line.c_str(), nullptr, 10);
        if (n < 0) {
            r.type = Reply::Type::Nil;
            return r;
        }
        r.type = Reply::Type::Array;
        r.array.reserve(static_cast<size_t>(n));
        for (long long i = 0; i < n; ++i) {
            r.array.push_back(parseReply(deadline));
        }
        return r;
    }
    default:
        r.type = Reply::Type::Error;
        r.str = "protocol error";
        return r;
    }
}

RedisConn::Reply RedisConn::command(const std::vector<std::string>& argv, int64_t timeoutMs)
{
    Reply err;
    if (fd_ < 0) {
        err.type = Reply::Type::Error;
        err.str = "not connected";
        return err;
    }
    std::string req = "*" + std::to_string(argv.size()) + "\r\n";
    for (size_t i = 0; i < argv.size(); ++i) {
        req += "$" + std::to_string(argv[i].size()) + "\r\n" + argv[i] + "\r\n";
    }
    const int64_t deadline = monotonicMs() + timeoutMs;
    if (!sendAll(req, deadline)) {
        close();
        err.type = Reply::Type::Error;
        err.str = "send failed";
        return err;
    }
    Reply r = parseReply(deadline);
    if (r.type == Reply::Type::Error) {
        close();
    }
    return r;
}
