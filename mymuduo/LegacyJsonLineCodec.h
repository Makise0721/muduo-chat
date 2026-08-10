#pragma once

#include <cstddef>
#include <string>

#include "StreamCodec.h"

class Buffer;

class LegacyJsonLineCodec : public OutputCodec
{
public:
    bool decode(Buffer *input, std::string *message);
    EncodeResult encode(const std::string &message, Buffer *output) const override;
    size_t encodedSize(size_t payloadBytes) const override;
};
