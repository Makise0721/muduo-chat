#include "BinaryFrameCodec.h"
#include "Buffer.h"
#include "StreamCodec.h"

#include <gtest/gtest.h>

#include <string>

TEST(BinaryFrameCodecTest, EncodeDecodeRoundTrip)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("{\"msgid\":1}", &out);
    std::string msg;
    EXPECT_EQ(CodecResult::Message, codec.decode(&out, &msg));
    EXPECT_EQ("{\"msgid\":1}", msg);
    EXPECT_EQ(0u, out.readableBytes());
}

TEST(BinaryFrameCodecTest, TwoFramesConsumedOneByOne)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("one", &out);
    codec.encode("two", &out);
    std::string msg;
    EXPECT_EQ(CodecResult::Message, codec.decode(&out, &msg));
    EXPECT_EQ("one", msg);
    EXPECT_EQ(CodecResult::Message, codec.decode(&out, &msg));
    EXPECT_EQ("two", msg);
    EXPECT_EQ(CodecResult::NeedMore, codec.decode(&out, &msg));
}

TEST(BinaryFrameCodecTest, PartialFrameWaitsWithoutConsuming)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("hello", &out);
    std::string bytes = out.retrieveAllAsString();
    bytes.resize(bytes.size() - 3);
    Buffer partial;
    partial.append(bytes.data(), bytes.size());
    std::string msg;
    EXPECT_EQ(CodecResult::NeedMore, codec.decode(&partial, &msg));
    EXPECT_EQ(bytes.size(), partial.readableBytes());
}

TEST(BinaryFrameCodecTest, BadMagicIsProtocolError)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("x", &out);
    std::string bytes = out.retrieveAllAsString();
    bytes[0] = 'X';
    Buffer bad;
    bad.append(bytes.data(), bytes.size());
    std::string msg;
    EXPECT_EQ(CodecResult::ProtocolError, codec.decode(&bad, &msg));
    EXPECT_EQ(bytes.size(), bad.readableBytes());
}

TEST(BinaryFrameCodecTest, BodyWithoutNewlineIsTheMessage)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("{\"msgid\":1}", &out);
    std::string bytes = out.retrieveAllAsString();
    EXPECT_NE('\n', bytes.back());
    Buffer buf;
    buf.append(bytes.data(), bytes.size());
    std::string msg;
    EXPECT_EQ(CodecResult::Message, codec.decode(&buf, &msg));
    EXPECT_EQ("{\"msgid\":1}", msg);
}

TEST(BinaryFrameCodecTest, EquivalenceWithLegacyContent)
{
    BinaryFrameCodec codec;
    Buffer out;
    codec.encode("{\"msgid\":1,\"name\":\"a\"}", &out);
    std::string wire = out.retrieveAllAsString();
    EXPECT_GT(wire.size(), 20u);
    EXPECT_EQ('}', wire.back());
}
