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
    while (buffer_.size() - offset_ < static_cast<qsizetype>(n))
    {
        const QByteArray chunk = poll_();
        if (!chunk.isEmpty())
        {
            // Compact the already-consumed prefix away now, on refill,
            // rather than shifting the whole tail on every take().
            if (offset_ > 0)
            {
                buffer_.remove(0, offset_);
                offset_ = 0;
            }
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

    const auto wanted = std::min<qsizetype>(static_cast<qsizetype>(n), buffer_.size() - offset_);
    QByteArray out = buffer_.mid(offset_, wanted);
    offset_ += wanted;
    if (offset_ == buffer_.size())
    {
        // Fully drained: reset so the next refill starts from an empty
        // buffer instead of compacting a now-pointless prefix.
        buffer_.clear();
        offset_ = 0;
    }
    return out;
}

void SerialByteBuffer::clear()
{
    buffer_.clear();
    offset_ = 0;
}

std::size_t SerialByteBuffer::buffered() const
{
    return static_cast<std::size_t>(buffer_.size() - offset_);
}
