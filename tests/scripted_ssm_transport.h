#pragma once
#include "protocol/issm_transport.h"
#include <QList>

// Test double: assert the exact sequence of writes, feed canned reads in order.
// Mirrors tests/scripted_kline_transport.h's shape.
class ScriptedSsmTransport : public ISsmTransport {
public:
    void expectWrite(const QByteArray &b) { expected_.append(b); }
    void queueRead(const QByteArray &b) { reads_.append(b); }
    bool scriptConsumed() const { return wIdx_ == expected_.size() && rIdx_ == reads_.size(); }
    bool ok() const { return ok_; }
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }

    int write(const QByteArray &data) override {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != data)
            ok_ = false;
        else
            ++wIdx_;
        return data.size();
    }

    QByteArray read(int) override {
        if (rIdx_ >= reads_.size())
            return QByteArray();
        return reads_.at(rIdx_++);
    }

private:
    QList<QByteArray> expected_, reads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
