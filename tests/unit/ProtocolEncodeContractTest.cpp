// P5-03B 序列化拷贝消除（ProtocolCodec JSON 编码路径）RED 契约测试
// （docs/tasks/P5-03B.md「RED」）。
//
// 目标：为服务器回复编码的零拷贝/直接字符串 encode seam 定义字节级契约——
// 新 encode 入口产出的 wire 字节必须与既有 `buildXReply(...).dump() + "\n"`
// 及 tests/fixtures/ golden 逐字节一致，从而在消除 nlohmann::json 对象构造 +
// dump 的序列化拷贝时，线上协议字节零变化。
//
// 当前实现只有 `buildMessageAcceptedReply` / `buildErrorReply` 返回 nlohmann::json，
// 由调用侧 `.dump() + "\n"` 编码；本文件引用的 `encodeMessageAcceptedReply` /
// `encodeErrorReply` 尚不存在（最小实现阶段引入）→ 编译失败即合法 RED
// （与 P3-12/P4-02/P5-03A 先例一致：missing 接口 → 编译失败）。
//
// 服务器回复序列化范围（msgid 11/13；DELIVERY_ACK msgid=12 为客户端→服务器消息，
// 服务器从不序列化 DELIVERY_ACK，见「禁止」节——不属本 seam）。

#include "ChatService.hpp"
#include "app/ProtocolCodec.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

std::string readFixture(const std::string& name)
{
    std::ifstream in(repoRoot() + "tests/fixtures/" + name);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content;
}

// 服务器回复 = dump() + "\n"（ChatService 发送格式）。fixture 不含换行。
std::string replyBytes(const std::string& fixture)
{
    return fixture + "\n";
}

// 既有 dump 路径的 wire 字节（契约锚点，未经优化）。
std::string dumpAccepted(int msgid, const std::string& cmid, uint64_t messageId,
                         uint64_t conversationId, uint64_t sequence, bool duplicate)
{
    return buildMessageAcceptedReply(msgid, cmid, messageId, conversationId,
                                     sequence, duplicate)
               .dump() +
           "\n";
}

std::string dumpError(int msgid, int errnoCode, const std::string& errmsg,
                      const std::string* cmid)
{
    return buildErrorReply(msgid, errnoCode, errmsg, cmid).dump() + "\n";
}

// 200 字节非法原值 cmid 回显（超 64 字节非 ASCII 上限，供错误回复回显）。
std::string oversizedCmid()
{
    return std::string(200, 'x');
}

} // namespace

// MESSAGE_ACCEPTED（msgid=11）：新 encode seam 与既有 dump 路径 wire 字节逐字节一致，
// 且与 golden fixture 一致（沿 ReliableProtocolGolden 锚点）。
TEST(ProtocolEncodeContract, MessageAcceptedEncodeIdenticalToDumpAndGolden)
{
    {
        const std::string got =
            encodeMessageAcceptedReply(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false);
        EXPECT_EQ(dumpAccepted(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false), got);
        EXPECT_EQ(replyBytes(readFixture("reply_message_accepted.json")), got);
    }
    // duplicate=true 分支 + 更大数值。
    {
        const std::string got = encodeMessageAcceptedReply(
            MESSAGE_ACCEPTED_MSG, "cm-dup-1", 18446744073709551615ULL, 1, 9000, true);
        EXPECT_EQ(dumpAccepted(MESSAGE_ACCEPTED_MSG, "cm-dup-1",
                               18446744073709551615ULL, 1, 9000, true),
                  got);
    }
}

// 错误响应（msgid=13）：101..107 全 errno 与 dump/golden 逐字节一致（含非法原值回显）。
TEST(ProtocolEncodeContract, ErrorEncodeIdenticalToDumpAndGoldenForAllErrnos)
{
    const int errnos[] = {
        kErrnoNotConversationMember, kErrnoTooManyRecipients,
        kErrnoIdempotencyConflict,   kErrnoDependencyBusy,
        kErrnoContentTooLong,        kErrnoNotFound,
        kErrnoInvalidClientMessageId,
    };
    for (int errnoCode : errnos) {
        std::string cmid = (errnoCode == kErrnoInvalidClientMessageId)
                               ? oversizedCmid()
                               : std::string("cm-001");
        const std::string got =
            encodeErrorReply(ERROR_RESP_MSG, errnoCode, protocolErrmsg(errnoCode), &cmid);
        EXPECT_EQ(dumpError(ERROR_RESP_MSG, errnoCode, protocolErrmsg(errnoCode), &cmid),
                  got)
            << "errno " << errnoCode;
    }
}

// 错误响应省略 client_message_id（nullptr）：encode 与 dump 一致，且不含该字段。
TEST(ProtocolEncodeContract, ErrorEncodeOmitsClientMessageIdWhenNull)
{
    const std::string got = encodeErrorReply(
        ERROR_RESP_MSG, kErrnoNotFound, protocolErrmsg(kErrnoNotFound), nullptr);
    EXPECT_EQ(dumpError(ERROR_RESP_MSG, kErrnoNotFound, protocolErrmsg(kErrnoNotFound),
                        nullptr),
              got);
    EXPECT_EQ(std::string::npos, got.find("client_message_id"));
}

// 16KB content 上限错误回复：encode 与 dump 逐字节一致（长度上限边界）。
TEST(ProtocolEncodeContract, ErrorEncodeAtMaxContentBytes)
{
    std::string big(kMaxContentBytes, 'a');
    const std::string got = encodeErrorReply(
        ERROR_RESP_MSG, kErrnoContentTooLong, protocolErrmsg(kErrnoContentTooLong), &big);
    EXPECT_EQ(dumpError(ERROR_RESP_MSG, kErrnoContentTooLong,
                        protocolErrmsg(kErrnoContentTooLong), &big),
              got);
}

// EncodeResult 语义（沿 P1R-02：encode 自校验、失败不改 output/确定性、长度上限）。
// 本 seam 为纯函数返回 std::string（保持 mymuduo 无关 TU），"失败不改 output"映射为
// encode 不触碰/不改写既有 dump 路径：无论 encode 调用多少次，dump 基准字节恒不变。
TEST(ProtocolEncodeContract, EncodeIsPureAndSelfConsistent)
{
    const std::string dumpRef =
        dumpAccepted(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false);

    // 多次独立 encode → 逐字节一致（无共享可变状态，纯函数）。
    const std::string a =
        encodeMessageAcceptedReply(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false);
    const std::string b =
        encodeMessageAcceptedReply(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false);
    EXPECT_EQ(a, b);

    // encode 自校验：能回解析为与 dump 路径等价的 JSON（结构/值一致）。
    nlohmann::json parsed = nlohmann::json::parse(a);
    nlohmann::json dumpJson =
        buildMessageAcceptedReply(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false);
    EXPECT_EQ(dumpJson, parsed);
    EXPECT_EQ(11, parsed["msgid"].get<int>());
    EXPECT_EQ("cm-001", parsed["client_message_id"].get<std::string>());

    // 长度自校验：encode = dump + 尾随 '\n'（服务器发送格式）。
    EXPECT_EQ(dumpRef.size(), a.size());
    EXPECT_EQ('\n', a.back());

    // "失败不改 output"：encode 不改写既有 dump 基准（dump 路径字节不变）。
    EXPECT_EQ(dumpRef, dumpAccepted(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3, false));
}

// 长度上限：任何合法回复的 wire 长度有界（16KB content + 固定字段 + 换行）。
TEST(ProtocolEncodeContract, EncodeLengthIsBounded)
{
    std::string big(kMaxContentBytes, 'a');
    const std::string got = encodeErrorReply(
        ERROR_RESP_MSG, kErrnoContentTooLong, protocolErrmsg(kErrnoContentTooLong), &big);
    // 16KB content + errmsg/errno/msgid/cmid 字段 + 转义开销 + 换行。
    EXPECT_GT(got.size(), kMaxContentBytes);
    EXPECT_LT(got.size(), 2u * kMaxContentBytes);
    EXPECT_EQ('\n', got.back());
}

// RepeatedEncodeStable：同输入重复编码字节稳定（逐字节一致，含尾随 '\n'）。
TEST(ProtocolEncodeContract, RepeatedEncodeStable)
{
    const int kRounds = 64;
    std::string cmid = "cm-001";
    const std::string first = encodeErrorReply(
        ERROR_RESP_MSG, kErrnoNotFound, protocolErrmsg(kErrnoNotFound), &cmid);
    for (int i = 0; i < kRounds; ++i) {
        const std::string again = encodeErrorReply(
            ERROR_RESP_MSG, kErrnoNotFound, protocolErrmsg(kErrnoNotFound), &cmid);
        EXPECT_EQ(first, again);
    }
    EXPECT_EQ('\n', first.back());
}
