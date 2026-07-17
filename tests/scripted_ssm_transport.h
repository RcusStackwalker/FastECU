#pragma once
#include "protocol/issm_transport.h"
#include "protocol/qt_bytes.h"
#include <QList>

// Test double: assert the exact sequence of writes, feed canned reads in order.
// Mirrors tests/scripted_kline_transport.h's shape.
class ScriptedSsmTransport : public ISsmTransport
{
  public:
    void expectWrite(const QByteArray& b)
    {
        expected_.append(bytes::fromQByteArray(b));
    }
    void expectWrite(bytes::ByteView b)
    {
        expected_.append(bytes::Bytes(b.begin(), b.end()));
    }
    void queueRead(const QByteArray& b)
    {
        reads_.append(bytes::fromQByteArray(b));
    }
    void queueRead(bytes::ByteView b)
    {
        reads_.append(bytes::Bytes(b.begin(), b.end()));
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expected_.size() && rIdx_ == reads_.size();
    }
    bool ok() const
    {
        return ok_;
    }
    void setOpen(bool open)
    {
        open_ = open;
    }
    bool isOpen() const override
    {
        return open_;
    }

    int write(bytes::ByteView data) override
    {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            ok_ = false;
        }
        else
        {
            ++wIdx_;
        }
        return static_cast<int>(data.size());
    }

    bytes::Bytes read(int) override
    {
        if (rIdx_ >= reads_.size())
        {
            return {};
        }
        return reads_.at(rIdx_++);
    }

  private:
    QList<bytes::Bytes> expected_, reads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
