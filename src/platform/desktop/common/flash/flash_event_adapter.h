#pragma once
#include <QObject>
#include <QString>

#include <string_view>

#include "src/backend/ports/event_sink.h"

namespace fastecu::flash
{

// Standalone Qt adapter for fastecu::IEventSink: translates the portable
// log/progress/notice calls an IFlashExecutor makes into this object's own
// Qt signals. Deliberately holds no reference to FlashWorker (or anything
// else) -- FlashWorker::run() connects these signals to its own
// logEvent/progressChanged signals via a real signal-to-signal connect(),
// rather than this class emitting a FlashWorker's signals directly. That
// independence is what lets it be constructed and exercised entirely on its
// own (see tests/test_flash_event_adapter.cpp), without a FlashWorker or a
// QThread anywhere in the picture -- and it's what makes this class reusable
// by every later per-family-tail worker, not just FlashWorker.
class QtEventSinkAdapter final : public QObject, public IEventSink
{
    Q_OBJECT

  public:
    using QObject::QObject;

    void log(LogLevel level, std::string_view message) override;
    void progress(int done, int total) override;
    void notice(std::string_view message) override;

  signals:
    void logEvent(int level, QString message);
    void progressChanged(int done, int total);
};

} // namespace fastecu::flash
