#include "LegacyJsonLineCodec.h"

#include "Buffer.h"

#include <cstring>

bool LegacyJsonLineCodec::decode(Buffer *input, std::string *message)
{
    for (;;)
    {
        const char *data = input->peek();
        const size_t len = input->readableBytes();
        if (len == 0)
        {
            return false;
        }

        const void *nl = memchr(data, '\n', len);
        if (nl == nullptr)
        {
            return false;
        }

        const size_t lineLen = static_cast<const char *>(nl) - data;
        std::string line(data, lineLen);
        input->retrieve(lineLen + 1);

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        *message = std::move(line);
        return true;
    }
}

EncodeResult LegacyJsonLineCodec::encode(const std::string &message, Buffer *output)
{
    output->append(message.data(), message.size());
    output->append("\n", 1);
    return EncodeResult::Ok;
}

size_t LegacyJsonLineCodec::encodedSize(size_t payloadBytes) const
{
    return payloadBytes + 1;
}
