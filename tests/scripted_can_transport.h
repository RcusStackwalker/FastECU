#pragma once
#include "protocol/ican_transport.h"
#include <QList>
namespace cdbg {
// Test double: assert the exact sequence of (id,payload) writes, feed canned
// (id,payload) reads in order.
class ScriptedCanTransport : public ICanTransport {
public:
    void expectWrite(quint32 id, const QByteArray &payload) { expectedIds_.append(id); expectedPayloads_.append(payload); }
    void queueRead(quint32 id, const QByteArray &payload) { readIds_.append(id); readPayloads_.append(payload); }
    bool scriptConsumed() const { return wIdx_ == expectedIds_.size() && rIdx_ == readIds_.size(); }
    bool ok() const { return ok_; }
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }
    int write(quint32 id, const QByteArray &payload) override {
        if (wIdx_ >= expectedIds_.size() || expectedIds_.at(wIdx_) != id || expectedPayloads_.at(wIdx_) != payload)
            ok_ = false;
        else
            ++wIdx_;
        return payload.size();
    }
    QByteArray read(int, quint32 &outId) override {
        if (rIdx_ >= readIds_.size())
            return QByteArray();
        outId = readIds_.at(rIdx_);
        return readPayloads_.at(rIdx_++);
    }
private:
    QList<quint32> expectedIds_, readIds_;
    QList<QByteArray> expectedPayloads_, readPayloads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
}
