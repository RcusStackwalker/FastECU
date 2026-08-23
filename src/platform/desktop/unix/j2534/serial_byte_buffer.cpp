#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"

#include <algorithm>
#include <utility>

SerialByteBuffer::SerialByteBuffer(PollFn poll, WaitFn wait, NowFn now)
    : poll_(std::move(poll)), wait_(std::move(wait)), now_(std::move(now))
{
}

QByteArray SerialByteBuffer::take(std::uint32_t n, std::uint16_t timeout_ms)
{
    std::uint64_t deadline = now_() + timeout_ms;
    while (buffer_.size() < static_cast<qsizetype>(n))
    {
        const QByteArray chunk = poll_();
        if (!chunk.isEmpty())
        {
            buffer_.append(chunk);
            // Silence timeout: any arrival buys another full window.
            deadline = now_() + timeout_ms;
            // Re-check the size before waiting again -- the chunk may already
            // have satisfied the request.
            continue;
        }
        if (now_() >= deadline)
        {
            break;
        }
        wait_(1);
    }

    const auto wanted = std::min<qsizetype>(static_cast<qsizetype>(n), buffer_.size());
    QByteArray out = buffer_.left(wanted);
    buffer_.remove(0, wanted);
    return out;
}

void SerialByteBuffer::clear()
{
    buffer_.clear();
}

std::size_t SerialByteBuffer::buffered() const
{
    return static_cast<std::size_t>(buffer_.size());
}
