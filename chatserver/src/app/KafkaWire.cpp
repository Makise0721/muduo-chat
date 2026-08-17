#include "app/KafkaWire.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

// P4-04：自 KafkaPublisher.cpp 匿名命名空间原样提取的 wire 原语实现（行为零
// 变化；编码/解码逻辑逐行同源，仅命名空间与 clientId 参数化差异）。

namespace kafka_wire {

namespace {

int64_t wallMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- 大端编码 ----

void appendI16(std::string* b, int16_t v)
{
    b->push_back(static_cast<char>((v >> 8) & 0xFF));
    b->push_back(static_cast<char>(v & 0xFF));
}

void appendI32(std::string* b, int32_t v)
{
    b->push_back(static_cast<char>((v >> 24) & 0xFF));
    b->push_back(static_cast<char>((v >> 16) & 0xFF));
    b->push_back(static_cast<char>((v >> 8) & 0xFF));
    b->push_back(static_cast<char>(v & 0xFF));
}

void appendI64(std::string* b, int64_t v)
{
    for (int i = 0; i < 8; ++i) {
        b->push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
    }
}

void patchI32(std::string* b, size_t at, int32_t v)
{
    (*b)[at] = static_cast<char>((v >> 24) & 0xFF);
    (*b)[at + 1] = static_cast<char>((v >> 16) & 0xFF);
    (*b)[at + 2] = static_cast<char>((v >> 8) & 0xFF);
    (*b)[at + 3] = static_cast<char>(v & 0xFF);
}

int32_t readI32(const char* b)
{
    return (static_cast<uint8_t>(b[0]) << 24) | (static_cast<uint8_t>(b[1]) << 16)
        | (static_cast<uint8_t>(b[2]) << 8) | static_cast<uint8_t>(b[3]);
}

// poll() 封装：遇 EINTR（信号中断）重试，不视为失败；其余返回码原样（有界：
// 每次调用仍受入参 timeout 约束）。
int pollRetry(struct pollfd* fds, nfds_t nfds, int timeoutMs)
{
    for (;;) {
        const int rc = ::poll(fds, nfds, timeoutMs);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc;
    }
}

// ---- CRC32C（Kafka magic-2 record batch 校验；标准 CRC-32C：初值 0xFFFFFFFF、
// 最终异或 0xFFFFFFFF，与 iSCSI/RFC3720 同源）----
// 查找表经函数局部 static const 一次性构造（C++11 magic static 保证线程安全）。

std::array<uint32_t, 256> buildCrc32cTable()
{
    std::array<uint32_t, 256> table = {};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

uint32_t crc32c(const char* data, size_t len)
{
    static const std::array<uint32_t, 256> table = buildCrc32cTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

TcpClient::~TcpClient()
{
    close();
}

bool TcpClient::connectTo(const std::string& host, int port, int64_t timeoutMs)
{
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
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
    setNonBlocking(true);
    int rc = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        close();
        return false;
    }
    if (rc != 0) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLOUT;
        p.revents = 0;
        rc = pollRetry(&p, 1, static_cast<int>(timeoutMs));
        if (rc <= 0 || (p.revents & (POLLERR | POLLHUP)) != 0) {
            close();
            return false;
        }
    }
    setNonBlocking(false);
    return true;
}

bool TcpClient::sendAll(const char* data, size_t len, int64_t timeoutMs)
{
    if (fd_ < 0) {
        return false;
    }
    size_t sent = 0;
    while (sent < len) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLOUT;
        p.revents = 0;
        if (pollRetry(&p, 1, static_cast<int>(timeoutMs)) <= 0) {
            return false;
        }
        ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TcpClient::recvExact(char* buf, size_t len, int64_t timeoutMs)
{
    if (fd_ < 0) {
        return false;
    }
    size_t got = 0;
    while (got < len) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        if (pollRetry(&p, 1, static_cast<int>(timeoutMs)) <= 0) {
            return false;
        }
        ssize_t n = ::recv(fd_, buf + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

void TcpClient::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void TcpClient::setNonBlocking(bool nb)
{
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    if (nb) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    (void)::fcntl(fd_, F_SETFL, flags);
}

void ByteWriter::i8(int8_t v)
{
    buf_.append(1, static_cast<char>(v));
}

void ByteWriter::i16(int16_t v)
{
    appendI16(&buf_, v);
}

void ByteWriter::i32(int32_t v)
{
    appendI32(&buf_, v);
}

void ByteWriter::i64(int64_t v)
{
    appendI64(&buf_, v);
}

void ByteWriter::str(const std::string& s)
{
    i16(static_cast<int16_t>(s.size()));
    buf_.append(s);
}

void ByteWriter::nullableStrNull()
{
    i16(-1);
}

void ByteWriter::bytes(const std::string& b)
{
    i32(static_cast<int32_t>(b.size()));
    buf_.append(b);
}

void ByteWriter::append(const std::string& s)
{
    buf_.append(s);
}

void ByteWriter::varint(int64_t v)
{
    uint64_t u = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    while (u >= 0x80) {
        buf_.push_back(static_cast<char>((u & 0x7F) | 0x80));
        u >>= 7;
    }
    buf_.push_back(static_cast<char>(u));
}

const std::string& ByteWriter::data() const
{
    return buf_;
}

ByteReader::ByteReader(const std::string& data) : data_(data)
{
}

bool ByteReader::i8(int8_t* out)
{
    if (remaining() < 1) {
        return false;
    }
    *out = static_cast<int8_t>(data_[pos_]);
    pos_ += 1;
    return true;
}

bool ByteReader::bool_(bool* out)
{
    int8_t v;
    if (!i8(&v)) {
        return false;
    }
    *out = v != 0;
    return true;
}

bool ByteReader::i16(int16_t* out)
{
    if (remaining() < 2) {
        return false;
    }
    *out = static_cast<int16_t>((uint8_t(data_[pos_]) << 8) | uint8_t(data_[pos_ + 1]));
    pos_ += 2;
    return true;
}

bool ByteReader::i32(int32_t* out)
{
    if (remaining() < 4) {
        return false;
    }
    *out = (uint8_t(data_[pos_]) << 24) | (uint8_t(data_[pos_ + 1]) << 16)
        | (uint8_t(data_[pos_ + 2]) << 8) | uint8_t(data_[pos_ + 3]);
    pos_ += 4;
    return true;
}

bool ByteReader::i64(int64_t* out)
{
    if (remaining() < 8) {
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | uint8_t(data_[pos_ + i]);
    }
    pos_ += 8;
    *out = static_cast<int64_t>(v);
    return true;
}

bool ByteReader::str(std::string* out)
{
    int16_t len;
    if (!i16(&len) || len < 0) {
        return false;
    }
    if (remaining() < static_cast<size_t>(len)) {
        return false;
    }
    out->assign(data_, pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
}

bool ByteReader::nullableStr(std::string* out, bool* isNull)
{
    int16_t len;
    if (!i16(&len)) {
        return false;
    }
    if (len < 0) {
        *isNull = true;
        out->clear();
        return true;
    }
    *isNull = false;
    if (remaining() < static_cast<size_t>(len)) {
        return false;
    }
    out->assign(data_, pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
}

bool ByteReader::bytes(std::string* out)
{
    int32_t len;
    if (!i32(&len) || len < 0) {
        return false;
    }
    if (remaining() < static_cast<size_t>(len)) {
        return false;
    }
    out->assign(data_, pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
}

bool ByteReader::nullableBytes(std::string* out, bool* isNull)
{
    int32_t len;
    if (!i32(&len)) {
        return false;
    }
    if (len < 0) {
        *isNull = true;
        out->clear();
        return true;
    }
    *isNull = false;
    if (remaining() < static_cast<size_t>(len)) {
        return false;
    }
    out->assign(data_, pos_, static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
}

void ByteReader::skip(size_t n)
{
    pos_ += n;
}

size_t ByteReader::remaining() const
{
    return data_.size() - pos_;
}

bool sendRequest(TcpClient* tcp, int16_t apiKey, int16_t apiVersion,
                 const std::string& clientId, const std::string& body, int32_t correlation,
                 int64_t timeoutMs)
{
    ByteWriter payload;
    payload.i16(apiKey);
    payload.i16(apiVersion);
    payload.i32(correlation);
    payload.str(clientId);
    payload.append(body);
    if (payload.data().size() > 0x7FFFFFFF) {
        return false;
    }
    ByteWriter frame;
    frame.i32(static_cast<int32_t>(payload.data().size()));
    const std::string wire = frame.data() + payload.data();
    return tcp->sendAll(wire.data(), wire.size(), timeoutMs);
}

bool recvResponse(TcpClient* tcp, int32_t correlation, std::string* body, int64_t timeoutMs)
{
    char sz[4];
    if (!tcp->recvExact(sz, 4, timeoutMs)) {
        return false;
    }
    const int32_t respSize = (uint8_t(sz[0]) << 24) | (uint8_t(sz[1]) << 16)
        | (uint8_t(sz[2]) << 8) | uint8_t(sz[3]);
    if (respSize < 4) {
        return false;
    }
    std::string resp;
    resp.resize(static_cast<size_t>(respSize));
    if (!tcp->recvExact(&resp[0], static_cast<size_t>(respSize), timeoutMs)) {
        return false;
    }
    const int32_t corr = readI32(resp.data());
    if (corr != correlation) {
        return false;
    }
    body->assign(resp, 4, resp.size() - 4);
    return true;
}

std::string encodeRecordBatch(const std::vector<std::pair<std::string, std::string> >& pairs)
{
    ByteWriter records;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const std::string& key = pairs[i].first;
        const std::string& value = pairs[i].second;
        ByteWriter rec;
        rec.varint(0);                      // attributes
        rec.varint(0);                      // timestampDelta
        rec.varint(static_cast<int64_t>(i));  // offsetDelta（保序 0..N-1）
        rec.varint(static_cast<int64_t>(key.size()));
        rec.append(key);
        rec.varint(static_cast<int64_t>(value.size()));
        rec.append(value);
        rec.varint(0);                      // headers count

        ByteWriter record;
        record.varint(static_cast<int64_t>(rec.data().size()));
        record.append(rec.data());
        records.append(record.data());
    }

    std::string b;
    b.append(8, '\0');                  // baseOffset = 0
    b.append(4, '\0');                  // batchLength 占位 [8..11]
    appendI32(&b, -1);                  // partitionLeaderEpoch [12..15]
    b.push_back(static_cast<char>(2));  // magic [16]
    b.append(4, '\0');                  // crc 占位 [17..20]
    appendI16(&b, 0);                   // attributes [21..22]
    appendI32(&b, static_cast<int32_t>(pairs.size() - 1));  // lastOffsetDelta [23..26]
    const int64_t ts = wallMs();
    appendI64(&b, ts);                  // firstTimestamp [27..34]
    appendI64(&b, ts);                  // maxTimestamp [35..42]
    appendI64(&b, -1);                  // producerId [43..50]
    appendI16(&b, -1);                  // producerEpoch [51..52]
    appendI32(&b, -1);                  // baseSequence [53..56]
    appendI32(&b, static_cast<int32_t>(pairs.size()));  // recordCount [57..60]
    b += records.data();                // records [61..]

    patchI32(&b, 8, static_cast<int32_t>(b.size() - 12));
    const uint32_t crc = crc32c(b.data() + 21, b.size() - 21);
    patchI32(&b, 17, static_cast<int32_t>(crc));
    return b;
}

bool readVarint(const std::string& data, size_t* pos, int64_t* out)
{
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (*pos >= data.size()) {
            return false;
        }
        const uint8_t b = static_cast<uint8_t>(data[*pos]);
        ++*pos;
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    const int64_t decoded = static_cast<int64_t>(result >> 1);
    *out = (result & 1) ? ~decoded : decoded;
    return true;
}

} // namespace kafka_wire
