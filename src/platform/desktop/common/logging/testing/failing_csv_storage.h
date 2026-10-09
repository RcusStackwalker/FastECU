#pragma once
#include <QFile>

namespace fastecu::desktop::logging::testing
{
class FailingCsvStorage : public QFile
{
  public:
    bool fail_writes = false;

  protected:
    qint64 writeData(const char *data, qint64 size) override
    {
        if (fail_writes)
        {
            setErrorString("simulated disk write failure");
            return -1;
        }
        return QFile::writeData(data, size);
    }
};
} // namespace fastecu::desktop::logging::testing
