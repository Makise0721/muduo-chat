#pragma once

#include <string>

#include "StreamCodec.h"

class Buffer;

enum class CodecResult
{
    Message,
    NeedMore,
    ProtocolError,
};

class BinaryFrameCodec
{
public:
    explicit BinaryFrameCodec(uint32_t maxBodyLength = StreamCodec::kDefaultMaxBodyLength);

    CodecResult decode(Buffer *input, std::string *message);
    void encode(const std::string &message, Buffer *output);

private:
    StreamCodec codec_;
};
