#pragma once

#include <string>

class Buffer;

class LegacyJsonLineCodec
{
public:
    bool decode(Buffer *input, std::string *message);
    void encode(const std::string &message, Buffer *output);
};
