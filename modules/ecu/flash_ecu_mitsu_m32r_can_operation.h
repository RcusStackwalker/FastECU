#ifndef FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
#define FLASH_ECU_MITSU_M32R_CAN_OPERATION_H

#include <QByteArray>
#include <QString>

#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuMitsuM32rCan (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
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
                                   QObject *parent = nullptr,
                                   PromptFn promptOverride = {});

protected:
    bool execute() override;

private:
    #define STATUS_SUCCESS  0x00
    #define STATUS_ERROR    0x01

    int connect_bootloader();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    bool upload_and_commit(uint32_t start, const QByteArray &data);
    QByteArray build_request(const QByteArray &sidPayload);
    QString parse_message_to_hex(QByteArray received);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;

    int mcu_type_index = 0;
    QString mcu_type_string;

    uint16_t serial_read_timeout = 500;
    uint16_t serial_read_extra_long_timeout = 3000;
};

#endif // FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
