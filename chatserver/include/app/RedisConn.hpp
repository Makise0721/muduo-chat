#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 最小 RESP（REdis Serialization Protocol）客户端，Redis presence adapter
// （P4-02）与测试 fixture 共用。Linux 专有（socket/poll）；同步请求/响应，
// 一次 command 一条 reply；超时/断连即视为依赖不可用（调用方映射）。
// 线程语义：单连接不跨线程共享；不同 RedisPresenceDirectory 实例各持一条连接。

class RedisConn {
public:
    struct Reply {
        enum class Type { Simple, Integer, Bulk, Error, Nil, Array };
        Type type = Type::Nil;
        int64_t integer = 0;
        std::string str;            // Simple（如 "OK"）/ Bulk 数据 / Error 消息
        std::vector<Reply> array;
        bool isError() const { return type == Type::Error; }
        const std::string& error() const { return str; }
    };

    RedisConn() = default;
    ~RedisConn();
    RedisConn(const RedisConn&) = delete;
    RedisConn& operator=(const RedisConn&) = delete;

    // 建立 TCP 连接并 SELECT db（db>0 才发送）；超时返回 false。
    bool connect(const std::string& host, int port, int db, int64_t timeoutMs);
    // 连接已建立且未因错误关闭。
    bool connected() const { return fd_ >= 0; }
    // 发送命令并解析一条回复；超时/断连 → 关闭连接并返回 Error 回复。
    Reply command(const std::vector<std::string>& argv, int64_t timeoutMs);
    void close();

private:
    bool sendAll(const std::string& data, int64_t timeoutMs);
    bool recvByte(char& c, int64_t timeoutMs);
    bool recvLine(std::string& line, int64_t timeoutMs);
    bool recvExact(std::string& out, size_t n, int64_t timeoutMs);
    Reply parseReply(int64_t timeoutMs);

    int fd_ = -1;
};
