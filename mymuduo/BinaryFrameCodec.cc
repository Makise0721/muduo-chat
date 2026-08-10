#include "BinaryFrameCodec.h"

#include "Buffer.h"

BinaryFrameCodec::BinaryFrameCodec(uint32_t maxBodyLength)
    : codec_(maxBodyLength),
      maxBodyLength_(maxBodyLength < StreamCodec::kHardMaxBodyLength
                         ? maxBodyLength
                         : StreamCodec::kHardMaxBodyLength)
{
}

CodecResult BinaryFrameCodec::decode(Buffer *input, std::string *message)
{
    Frame frame;
    const DecodeResult r = codec_.decode(input, &frame);
    switch (r)
    {
    case DecodeResult::FrameReady:
        message->assign(frame.body.begin(), frame.body.end());
        return CodecResult::Message;
    case DecodeResult::NeedMore:
        return CodecResult::NeedMore;
    default:
        return CodecResult::ProtocolError;
    }
}

EncodeResult BinaryFrameCodec::encode(const std::string &message, Buffer *output) const
{
    if (message.size() > maxBodyLength_)
    {
        return EncodeResult::TooLarge;
    }
    Frame frame;
    frame.magic = StreamCodec::kMagic;
    frame.version = StreamCodec::kVersion;
    frame.headerLength = StreamCodec::kHeaderLength;
    frame.bodyLength = static_cast<uint32_t>(message.size());
    frame.messageType = 1;
    frame.contentType = StreamCodec::kContentTypeJson;
    frame.reserved = 0;
    frame.requestId = 0;
    frame.body.assign(message.begin(), message.end());
    return codec_.encode(frame, output);
}

size_t BinaryFrameCodec::encodedSize(size_t payloadBytes) const
{
    if (payloadBytes > maxBodyLength_)
    {
        return static_cast<size_t>(-1);
    }
    return StreamCodec::kHeaderLength + payloadBytes;
}
