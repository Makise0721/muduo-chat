#pragma once

#include <cstddef>
#include <string>

#include "StreamCodec.h"

class Buffer;

enum class CodecResult
{
    Message,
    NeedMore,
    ProtocolError,
};

class BinaryFrameCodec : public OutputCodec
{
public:
    explicit BinaryFrameCodec(uint32_t maxBodyLength = StreamCodec::kDefaultMaxBodyLength);

    CodecResult decode(Buffer *input, std::string *message);
    EncodeResult encode(const std::string &message, Buffer *output) override;
    size_t encodedSize(size_t payloadBytes) const override;

private:
    StreamCodec codec_;
    uint32_t maxBodyLength_;
};
