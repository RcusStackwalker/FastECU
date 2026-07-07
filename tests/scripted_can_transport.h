#pragma once
#include "protocol/ican_transport.h"
#include "protocol/qt_bytes.h"
#include <QList>

#include <cstdint>
namespace cdbg {
// Test double: assert the exact sequence of (id,payload) writes, feed canned
// (id,payload) reads in order.
class ScriptedCanTransport : public ICanTransport {
public:
    void expectWrite(std::uint32_t id, const QByteArray &payload) { expectedIds_.append(id); expectedPayloads_.append(bytes::fromQByteArray(payload)); }
    void expectWrite(std::uint32_t id, bytes::ByteView payload) { expectedIds_.append(id); expectedPayloads_.append(bytes::Bytes(payload.begin(), payload.end())); }
    void queueRead(std::uint32_t id, const QByteArray &payload) { readIds_.append(id); readPayloads_.append(bytes::fromQByteArray(payload)); }
    void queueRead(std::uint32_t id, bytes::ByteView payload) { readIds_.append(id); readPayloads_.append(bytes::Bytes(payload.begin(), payload.end())); }
    bool scriptConsumed() const { return wIdx_ == expectedIds_.size() && rIdx_ == readIds_.size(); }
    bool ok() const { return ok_; }
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }
    int write(std::uint32_t id, bytes::ByteView payload) override {
        if (wIdx_ >= expectedIds_.size() || expectedIds_.at(wIdx_) != id || expectedPayloads_.at(wIdx_) != bytes::Bytes(payload.begin(), payload.end()))
            ok_ = false;
        else
            ++wIdx_;
        return static_cast<int>(payload.size());
    }
    bytes::Bytes read(int, std::uint32_t &outId) override {
        if (rIdx_ >= readIds_.size())
            return {};
        outId = readIds_.at(rIdx_);
        return readPayloads_.at(rIdx_++);
    }
private:
    QList<std::uint32_t> expectedIds_, readIds_;
    QList<bytes::Bytes> expectedPayloads_, readPayloads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
}
