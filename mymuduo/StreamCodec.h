#pragma once

#include <cstdint>
#include <vector>

class Buffer;

enum class DecodeResult
{
    NeedMore,
    FrameReady,
    UnsupportedVersion,
    ProtocolError,
};

struct Frame
{
    uint32_t magic = 0;
    uint8_t version = 0;
    uint8_t flags = 0;
    uint16_t headerLength = 0;
    uint32_t bodyLength = 0;
    uint16_t messageType = 0;
    uint8_t contentType = 0;
    uint8_t reserved = 0;
    uint32_t requestId = 0;
    std::vector<char> body;
};

class StreamCodec
{
public:
    static const uint32_t kMagic;
    static const uint8_t kVersion;
    static const uint16_t kHeaderLength;
    static const uint32_t kDefaultMaxBodyLength;
    static const uint32_t kHardMaxBodyLength;
    static const uint8_t kContentTypeJson;

    explicit StreamCodec(uint32_t maxBodyLength = kDefaultMaxBodyLength);

    DecodeResult decode(Buffer *input, Frame *frame);
    void encode(const Frame &frame, Buffer *output);

private:
    uint32_t maxBodyLength_;
};
