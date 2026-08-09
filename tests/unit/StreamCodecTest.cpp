#include "Buffer.h"
#include "StreamCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

Buffer makeFrameBytes(uint8_t version,
                      uint8_t flags,
                      uint16_t headerLength,
                      uint32_t bodyLength,
                      uint16_t messageType,
                      uint8_t contentType,
                      uint8_t reserved,
                      uint32_t requestId,
                      const std::string &body = "")
{
    Buffer buf;
    uint8_t header[20] = {0};
    header[0] = 0x4D;
    header[1] = 0x43;
    header[2] = 0x48;
    header[3] = 0x54;
    header[4] = version;
    header[5] = flags;
    header[6] = static_cast<uint8_t>(headerLength >> 8);
    header[7] = static_cast<uint8_t>(headerLength & 0xFF);
    header[8] = static_cast<uint8_t>(bodyLength >> 24);
    header[9] = static_cast<uint8_t>((bodyLength >> 16) & 0xFF);
    header[10] = static_cast<uint8_t>((bodyLength >> 8) & 0xFF);
    header[11] = static_cast<uint8_t>(bodyLength & 0xFF);
    header[12] = static_cast<uint8_t>(messageType >> 8);
    header[13] = static_cast<uint8_t>(messageType & 0xFF);
    header[14] = contentType;
    header[15] = reserved;
    header[16] = static_cast<uint8_t>(requestId >> 24);
    header[17] = static_cast<uint8_t>((requestId >> 16) & 0xFF);
    header[18] = static_cast<uint8_t>((requestId >> 8) & 0xFF);
    header[19] = static_cast<uint8_t>(requestId & 0xFF);
    buf.append(reinterpret_cast<const char *>(header), sizeof header);
    if (!body.empty())
    {
        buf.append(body.data(), body.size());
    }
    return buf;
}

} // namespace

TEST(StreamCodecTest, EncodeGoldenBytes)
{
    StreamCodec codec;
    Frame frame;
    frame.magic = 0x4D434854;
    frame.version = 2;
    frame.flags = 0;
    frame.headerLength = 20;
    frame.bodyLength = 4;
    frame.messageType = 1;
    frame.contentType = 1;
    frame.reserved = 0;
    frame.requestId = 0x01020304;
    frame.body = {'a', 'b', 'c', 'd'};
    Buffer out;
    codec.encode(frame, &out);
    std::string got = out.retrieveAllAsString();
    const std::string expect(
        "\x4D\x43\x48\x54"
        "\x02\x00"
        "\x00\x14"
        "\x00\x00\x00\x04"
        "\x00\x01"
        "\x01\x00"
        "\x01\x02\x03\x04"
        "abcd",
        24);
    EXPECT_EQ(expect, got);
}

TEST(StreamCodecTest, DecodeGoldenBytes)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 4, 1, 1, 0, 0x01020304, "abcd");
    Frame frame;
    EXPECT_EQ(DecodeResult::FrameReady, codec.decode(&buf, &frame));
    EXPECT_EQ(0x4D434854u, frame.magic);
    EXPECT_EQ(2u, frame.version);
    EXPECT_EQ(0u, frame.flags);
    EXPECT_EQ(20u, frame.headerLength);
    EXPECT_EQ(4u, frame.bodyLength);
    EXPECT_EQ(1u, frame.messageType);
    EXPECT_EQ(1u, frame.contentType);
    EXPECT_EQ(0u, frame.reserved);
    EXPECT_EQ(0x01020304u, frame.requestId);
    EXPECT_EQ(std::string("abcd"), std::string(frame.body.begin(), frame.body.end()));
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(StreamCodecTest, HeaderTruncationReturnsNeedMoreWithoutConsuming)
{
    StreamCodec codec;
    for (size_t n = 0; n < 20; ++n)
    {
        Buffer buf = makeFrameBytes(2, 0, 20, 0, 1, 1, 0, 0);
        std::string bytes = buf.retrieveAllAsString();
        bytes.resize(n);
        Buffer partial;
        partial.append(bytes.data(), bytes.size());
        Frame frame;
        EXPECT_EQ(DecodeResult::NeedMore, codec.decode(&partial, &frame));
        EXPECT_EQ(n, partial.readableBytes());
    }
}

TEST(StreamCodecTest, PartialBodyReturnsNeedMoreWithoutConsuming)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 10, 1, 1, 0, 0, std::string(5, 'x'));
    Frame frame;
    EXPECT_EQ(DecodeResult::NeedMore, codec.decode(&buf, &frame));
    EXPECT_EQ(25u, buf.readableBytes());
}

TEST(StreamCodecTest, TwoFramesInOneBufferConsumesOneFrame)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 2, 1, 1, 0, 1, "hi");
    Buffer second = makeFrameBytes(2, 0, 20, 1, 2, 1, 0, 2, "z");
    std::string secondBytes = second.retrieveAllAsString();
    buf.append(secondBytes.data(), secondBytes.size());
    Frame frame;
    EXPECT_EQ(DecodeResult::FrameReady, codec.decode(&buf, &frame));
    EXPECT_EQ(1u, frame.requestId);
    EXPECT_EQ(std::string("hi"), std::string(frame.body.begin(), frame.body.end()));
    EXPECT_EQ(21u, buf.readableBytes());
    EXPECT_EQ(DecodeResult::FrameReady, codec.decode(&buf, &frame));
    EXPECT_EQ(2u, frame.requestId);
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(StreamCodecTest, ZeroLengthBodyIsValid)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 0, 1, 1, 0, 7);
    Frame frame;
    EXPECT_EQ(DecodeResult::FrameReady, codec.decode(&buf, &frame));
    EXPECT_EQ(0u, frame.body.size());
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(StreamCodecTest, BodyAtDefaultLimitIsAccepted)
{
    StreamCodec codec;
    std::string body(1024 * 1024, 'x');
    Buffer buf = makeFrameBytes(2, 0, 20, 1024 * 1024, 1, 1, 0, 0, body);
    Frame frame;
    EXPECT_EQ(DecodeResult::FrameReady, codec.decode(&buf, &frame));
    EXPECT_EQ(body.size(), frame.body.size());
}

TEST(StreamCodecTest, BodyOverLimitIsProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 1024 * 1024 + 1, 1, 1, 0, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, BodyOverHardLimitIsProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 16 * 1024 * 1024 + 1, 1, 1, 0, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, BadMagicIsProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 0, 1, 1, 0, 0);
    std::string bytes = buf.retrieveAllAsString();
    bytes[0] = 'X';
    Buffer bad;
    bad.append(bytes.data(), bytes.size());
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&bad, &frame));
}

TEST(StreamCodecTest, UnsupportedVersionIsUnsupportedVersion)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(1, 0, 20, 0, 1, 1, 0, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::UnsupportedVersion, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, BadHeaderLengthIsProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 21, 0, 1, 1, 0, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, UndefinedFlagsAreProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0x01, 20, 0, 1, 1, 0, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, NonZeroReservedIsProtocolError)
{
    StreamCodec codec;
    Buffer buf = makeFrameBytes(2, 0, 20, 0, 1, 1, 1, 0);
    Frame frame;
    EXPECT_EQ(DecodeResult::ProtocolError, codec.decode(&buf, &frame));
}

TEST(StreamCodecTest, RandomInputStormDoesNotCrash)
{
    StreamCodec codec;
    std::mt19937 rng(20260809);
    for (int i = 0; i < 10000; ++i)
    {
        size_t len = rng() % 512;
        std::string blob(len, '\0');
        for (size_t j = 0; j < len; ++j)
        {
            blob[j] = static_cast<char>(rng() & 0xFF);
        }
        Buffer buf;
        buf.append(blob.data(), blob.size());
        Frame frame;
        DecodeResult r = codec.decode(&buf, &frame);
        EXPECT_TRUE(r == DecodeResult::NeedMore || r == DecodeResult::FrameReady ||
                    r == DecodeResult::UnsupportedVersion ||
                    r == DecodeResult::ProtocolError);
    }
}
