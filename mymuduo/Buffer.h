#pragma once

#include <vector>
#include <string>
#include <algorithm>

class Buffer
{
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readIndex_(kCheapPrepend),
          writeIndex_(kCheapPrepend)
    {
    }

    size_t readableBytes() const
    {
        return writeIndex_ - readIndex_;
    }
    size_t writableBytes() const
    {
        return buffer_.size() - writeIndex_;
    }
    size_t prependableBytes() const
    {
        return readIndex_;
    }

    const char *peek()
    {
        return begin() + readIndex_;
    }

    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readIndex_ += len;
        }
        else
        {
            retrieveAll();
        }
    }

    void retrieveAll()
    {
        readIndex_ = kCheapPrepend;
        writeIndex_ = kCheapPrepend;
    }

    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes());
    }

    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len);
        }
    }

    void append(const char *data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data + len, begin() + writeIndex_);
        writeIndex_ += len;
    }

    ssize_t readFd(int fd, int *savedErrno);
    ssize_t writeFd(int fd, int *savedErrno);

private:
    char *begin()
    {
        return buffer_.data();
    }

    void makeSpace(size_t len)
    {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writeIndex_ + len);
        }
        else
        {
            size_t readable = readableBytes();
            std::copy(begin() + readIndex_,
                      begin() + writeIndex_,
                      begin() + kCheapPrepend);
            readIndex_ = kCheapPrepend;
            writeIndex_ = readIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readIndex_;
    size_t writeIndex_;
};