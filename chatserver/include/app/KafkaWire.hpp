#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// P4-04 内部 wire seam（docs/tasks/P4-04.md D1/待定② 默认方案）：从 KafkaPublisher.cpp
// 匿名命名空间提取的共享 Kafka wire 原语——TcpClient（非阻塞 connect + 有界 poll）、
// 请求/响应帧（correlation 匹配）、ByteWriter/ByteReader（大端）、varint（zigzag）、
// magic-2 record batch 编码。KafkaPublisher（Produce 侧）与 KafkaEventConsumer
//（Fetch/OffsetFetch/OffsetCommit 消费侧）共用；行为零变化（P4-03 回归作证）。
// 内部实现细节（API 编解码、故障分类）留在各自 adapter .cpp。
namespace kafka_wire {

// TCP 客户端（非阻塞 connect + 有界 poll；断连/超时返回 false）。
class TcpClient {
public:
    TcpClient() = default;
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connectTo(const std::string& host, int port, int64_t timeoutMs);
    bool sendAll(const char* data, size_t len, int64_t timeoutMs);
    bool recvExact(char* buf, size_t len, int64_t timeoutMs);
    void close();
    bool isOpen() const { return fd_ >= 0; }

private:
    void setNonBlocking(bool nb);

    int fd_ = -1;
};

// 大端编码 writer（str = int16 长度前缀字符串；bytes = int32 长度前缀字节串；
// varint = 有符号 zigzag varint，record batch 内字段）。
class ByteWriter {
public:
    void i8(int8_t v);
    void i16(int16_t v);
    void i32(int32_t v);
    void i64(int64_t v);
    void str(const std::string& s);
    void nullableStrNull();
    void bytes(const std::string& b);
    void append(const std::string& s);
    void varint(int64_t v);
    const std::string& data() const;

private:
    std::string buf_;
};

// 大端解码 reader（布尔 = 1 字节非零；nullable 变体长度 -1 表示 null）。
class ByteReader {
public:
    explicit ByteReader(const std::string& data);

    bool i8(int8_t* out);
    bool bool_(bool* out);
    bool i16(int16_t* out);
    bool i32(int32_t* out);
    bool i64(int64_t* out);
    bool str(std::string* out);
    bool nullableStr(std::string* out, bool* isNull);
    bool bytes(std::string* out);
    bool nullableBytes(std::string* out, bool* isNull);
    void skip(size_t n);
    size_t remaining() const;

private:
    std::string data_;
    size_t pos_ = 0;
};

// 请求帧：payload = [apiKey, apiVersion, correlation, clientId, body]，前置 int32
// 长度。发送 + 接收响应帧并匹配 correlation（respBody 不含 correlation 头）。
bool sendRequest(TcpClient* tcp, int16_t apiKey, int16_t apiVersion,
                 const std::string& clientId, const std::string& body, int32_t correlation,
                 int64_t timeoutMs);
bool recvResponse(TcpClient* tcp, int32_t correlation, std::string* body, int64_t timeoutMs);

// magic-2 record batch：单 batch 含 pairs.size() 条 records（offsetDelta=0..N-1、
// lastOffsetDelta=N-1、recordCount=N）。crc 覆盖 attributes（offset 21）起至 batch
// 末尾。broker 拒绝单次 produce 含多个 record batch（magic-2 约束），同一
// (topic, partition) 的全部事件必须合并进单个 batch（P4-03 冻结）。
std::string encodeRecordBatch(const std::vector<std::pair<std::string, std::string> >& pairs);

// 有符号 varint（zigzag）解码，record batch 解析共用（KafkaTestConsumer 先例）。
bool readVarint(const std::string& data, size_t* pos, int64_t* out);

} // namespace kafka_wire
