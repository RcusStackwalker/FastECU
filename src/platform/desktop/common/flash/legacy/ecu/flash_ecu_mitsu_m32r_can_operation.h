#pragma once

#include <QByteArray>
#include <QString>

#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/flash/legacy/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuMitsuM32rCan (worker-thread migration).
// Owns every serial-> call and the Mitsubishi Colt CZT (Z37A) M32R CAN
// bootloader protocol sequence; relocated verbatim from
// FlashEcuMitsuM32rCan's former private methods.
class FlashEcuMitsuM32rCanOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuMitsuM32rCanOperation(SerialPortActions *serial,
                                  FileActions::EcuCalDefStructure *ecuCalDef,
                                  QString cmd_type,
                                  QWidget *dialog,
                                  bool useVendorChallenge = false,
                                  QObject *parent = nullptr,
                                  PromptFn promptOverride = {});

  protected:
    bool execute() override;

    // Protected, not private, so the unit test can assert on the real
    // implementation: this prepends the 4-byte big-endian ISO-15765 source
    // address that every request on this bus carries, and the path cannot be
    // bench-tested, so the wire format needs production-code coverage.
    QByteArray build_request(const QByteArray& sidPayload);

  private:
#define STATUS_SUCCESS 0x00
#define STATUS_ERROR 0x01

    int connect_bootloader();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    bool upload_and_commit(uint32_t start, const QByteArray& data);
    bool readFlashRange(uint32_t start_addr, uint32_t length, QByteArray *outData);
    bool ensureTopRegionWritten(const QByteArray& romdata);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
    bool useVendorChallenge = false;

    int mcu_type_index = 0;
    QString mcu_type_string;

    uint16_t serial_read_timeout = 500;
    uint16_t serial_read_extra_long_timeout = 3000;
};
