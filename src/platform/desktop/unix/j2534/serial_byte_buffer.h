#pragma once

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <functional>

// Staging buffer between the Openport's byte-oriented Tactrix protocol parser
// and the serial port.
//
// PassThruReadMsgs scans for '\r' / '\n' / ' ' terminators one byte at a time
// and genuinely cannot be batched -- it does not know a message's length until
// it has parsed the header. That parser is left exactly as written; what
// changes is the cost of a one-byte read. Previously each one cost a
// QSerialPort::read(1) plus a waitForReadyRead(1) that blocked its full
// millisecond whenever Qt had already drained the kernel buffer, so receiving
// ~200 bytes cost ~200 ms regardless of line rate. Here, one bulk refill
// serves many single-byte reads from memory.
//
// Bytes read past what a call asked for are retained, which is what makes bulk
// refill safe under a terminator-scanning parser: over-reading never loses
// data.
class SerialByteBuffer
{
  public:
    // Returns whatever is available right now, without blocking. Empty means
    // "nothing yet" and is not an error.
    using PollFn = std::function<QByteArray()>;
    // Blocks up to `ms` waiting for new data. Any return value is ignored --
    // the next poll decides what actually landed.
    using WaitFn = std::function<void(int ms)>;
    // Monotonic milliseconds. Injected so the timeout rules are testable
    // without sleeping.
    using NowFn = std::function<std::uint64_t()>;

    SerialByteBuffer(PollFn poll, WaitFn wait, NowFn now);

    // Returns up to `n` bytes, blocking until `n` are available or
    // `timeout_ms` passes with nothing arriving.
    //
    // The timeout is a SILENCE timeout, not a total budget: the deadline
    // refreshes on every arrival. This is deliberate and load-bearing --
    // read_serial_data(1, 200) has always meant "one byte, or 200 ms of
    // nothing", and changing it would change how long the app waits on a slow
    // adapter.
    //
    // A short return (including empty) is a normal timeout, not an error.
    QByteArray take(std::uint32_t n, std::uint16_t timeout_ms);

    // Drops retained bytes. Call across an open/close so stale bytes from a
    // previous connection cannot be parsed as a new message.
    void clear();

    std::size_t buffered() const;

  private:
    PollFn poll_;
    WaitFn wait_;
    NowFn now_;
    // buffer_[offset_..) is the retained-but-unconsumed data. A read offset
    // (rather than erasing consumed bytes on every take) keeps the common
    // case -- one bulk refill followed by many take(1) calls scanning for a
    // terminator -- O(1) per call instead of O(size) per call. The prefix is
    // compacted away only when new data is appended.
    QByteArray buffer_;
    qsizetype offset_ = 0;
};
