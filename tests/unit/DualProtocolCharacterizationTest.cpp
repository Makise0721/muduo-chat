#include "Buffer.h"
#include "BinaryFrameCodec.h"
#include "LegacyJsonLineCodec.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// v1/v2 业务等价的前置契约（行为矩阵 B-01..B-24 的传输层支撑）：
// 同一 JSON payload 在两个协议族下往返后必须还原为同一内容。
TEST(DualProtocolCharacterizationTest, SamePayloadRoundTripsToSameJson)
{
    const std::vector<std::string> payloads = {
        "{\"msgid\":4,\"name\":\"a\",\"password\":\"p\"}",
        "{\"msgid\":1,\"id\":42,\"password\":\"p\"}",
        "{\"msgid\":6,\"id\":1,\"toid\":2,\"msg\":\"hello\",\"time\":\"2024-01-01 12:00:00\"}",
    };
    for (const std::string& payload : payloads)
    {
        LegacyJsonLineCodec v1;
        Buffer v1out;
        ASSERT_EQ(EncodeResult::Ok, v1.encode(payload, &v1out));
        std::string v1msg;
        ASSERT_TRUE(v1.decode(&v1out, &v1msg));
        EXPECT_EQ(payload, v1msg);

        BinaryFrameCodec v2;
        Buffer v2out;
        ASSERT_EQ(EncodeResult::Ok, v2.encode(payload, &v2out));
        std::string v2msg;
        ASSERT_EQ(CodecResult::Message, v2.decode(&v2out, &v2msg));
        EXPECT_EQ(payload, v2msg);
    }
}

// 行为矩阵 B-23 的 v2 侧：超限帧必须被拒且不产生输出；v1 无对应限制。
TEST(DualProtocolCharacterizationTest, OversizedPayloadRejectedOnlyByV2)
{
    const std::string big(1 * 1024 * 1024 + 1, 'x');
    LegacyJsonLineCodec v1;
    Buffer v1out;
    EXPECT_EQ(EncodeResult::Ok, v1.encode(big, &v1out));
    EXPECT_EQ(big.size() + 1u, v1out.readableBytes());

    BinaryFrameCodec v2;
    Buffer v2out;
    EXPECT_EQ(EncodeResult::TooLarge, v2.encode(big, &v2out));
    EXPECT_EQ(0u, v2out.readableBytes());
}
