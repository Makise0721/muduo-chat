#include "StreamCodec.h"

#include "Buffer.h"

namespace {

uint16_t readBe16(const char *p)
{
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) |
                                 static_cast<uint8_t>(p[1]));
}

uint32_t readBe32(const char *p)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

void writeBe16(char *p, uint16_t v)
{
    p[0] = static_cast<char>((v >> 8) & 0xFF);
    p[1] = static_cast<char>(v & 0xFF);
}

void writeBe32(char *p, uint32_t v)
{
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

} // namespace

const uint32_t StreamCodec::kMagic = 0x4D434854;
const uint8_t StreamCodec::kVersion = 2;
const uint16_t StreamCodec::kHeaderLength = 20;
const uint32_t StreamCodec::kDefaultMaxBodyLength = 1024 * 1024;
const uint32_t StreamCodec::kHardMaxBodyLength = 16 * 1024 * 1024;
const uint8_t StreamCodec::kContentTypeJson = 1;

StreamCodec::StreamCodec(uint32_t maxBodyLength)
    : maxBodyLength_(maxBodyLength < kHardMaxBodyLength ? maxBodyLength
                                                        : kHardMaxBodyLength)
{
}

DecodeResult StreamCodec::decode(Buffer *input, Frame *frame)
{
    if (input->readableBytes() < kHeaderLength)
    {
        return DecodeResult::NeedMore;
    }

    const char *p = input->peek();
    const uint32_t magic = readBe32(p);
    const uint8_t version = static_cast<uint8_t>(p[4]);
    const uint8_t flags = static_cast<uint8_t>(p[5]);
    const uint16_t headerLength = readBe16(p + 6);
    const uint32_t bodyLength = readBe32(p + 8);
    const uint16_t messageType = readBe16(p + 12);
    const uint8_t contentType = static_cast<uint8_t>(p[14]);
    const uint8_t reserved = static_cast<uint8_t>(p[15]);
    const uint32_t requestId = readBe32(p + 16);

    if (magic != kMagic)
    {
        return DecodeResult::ProtocolError;
    }
    if (version != kVersion)
    {
        return DecodeResult::UnsupportedVersion;
    }
    if (headerLength != kHeaderLength)
    {
        return DecodeResult::ProtocolError;
    }
    if (flags != 0)
    {
        return DecodeResult::ProtocolError;
    }
    if (reserved != 0)
    {
        return DecodeResult::ProtocolError;
    }
    if (bodyLength > maxBodyLength_)
    {
        return DecodeResult::ProtocolError;
    }
    if (input->readableBytes() < kHeaderLength + bodyLength)
    {
        return DecodeResult::NeedMore;
    }

    frame->magic = magic;
    frame->version = version;
    frame->flags = flags;
    frame->headerLength = headerLength;
    frame->bodyLength = bodyLength;
    frame->messageType = messageType;
    frame->contentType = contentType;
    frame->reserved = reserved;
    frame->requestId = requestId;
    frame->body.assign(p + kHeaderLength, p + kHeaderLength + bodyLength);
    input->retrieve(kHeaderLength + bodyLength);
    return DecodeResult::FrameReady;
}

void StreamCodec::encode(const Frame &frame, Buffer *output)
{
    char header[20] = {0};
    writeBe32(header, frame.magic);
    header[4] = static_cast<char>(frame.version);
    header[5] = static_cast<char>(frame.flags);
    writeBe16(header + 6, frame.headerLength);
    writeBe32(header + 8, frame.bodyLength);
    writeBe16(header + 12, frame.messageType);
    header[14] = static_cast<char>(frame.contentType);
    header[15] = static_cast<char>(frame.reserved);
    writeBe32(header + 16, frame.requestId);
    output->append(header, sizeof header);
    if (!frame.body.empty())
    {
        output->append(frame.body.data(), frame.body.size());
    }
}
