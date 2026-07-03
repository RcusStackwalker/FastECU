#ifndef FLASH_ECU_SUBARU_DENSO_SH72543_CAN_DIESEL_OPERATION_H
#define FLASH_ECU_SUBARU_DENSO_SH72543_CAN_DIESEL_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruDensoSH72543CanDiesel (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
// Owns every serial-> call and the Subaru Denso SH72543 Diesel CAN
// bootloader protocol sequence; relocated verbatim from
// FlashEcuSubaruDensoSH72543CanDiesel's former private methods.
class FlashEcuSubaruDensoSH72543CanDieselOperation : public FlashOperationWorker
{
    Q_OBJECT

public:
    FlashEcuSubaruDensoSH72543CanDieselOperation(SerialPortActions *serial,
                                                  FileActions::EcuCalDefStructure *ecuCalDef,
                                                  QString cmd_type,
                                                  QWidget *dialog,
                                                  QObject *parent = nullptr,
                                                  PromptFn promptOverride = {});

protected:
    bool execute() override;

private:
    #define STATUS_SUCCESS	0x00
    #define STATUS_ERROR	0x01

    bool kernel_alive = false;
    bool test_write = false;
    bool request_denso_kernel_init = false;
    bool request_denso_kernel_id = false;

    int result;
    int mcu_type_index;
    int bootloader_start_countdown = 3;

    uint8_t tester_id;
    uint8_t target_id;

    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    uint32_t flashbytescount = 0;
    uint32_t flashbytesindex = 0;

    QString mcu_type_string;
    QString flash_method;
    QString kernel;

    int connect_bootloader();
    int read_memory(uint32_t start_addr, uint32_t length);
    int write_memory(bool test_write);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);
    int erase_memory(const struct flashdev_t *fdt, unsigned blockno);

    QByteArray generate_can_seed_key(QByteArray requested_seed);
    QByteArray calculate_seed_key(QByteArray requested_seed, const uint16_t *keytogenerateindex, const uint8_t *indextransformation);
    QByteArray encrypt_payload(QByteArray buf, uint32_t len);
    QByteArray decrypt_payload(QByteArray buf, uint32_t len);
    QByteArray calculate_payload(QByteArray buf, uint32_t len, const uint16_t *keytogenerateindex, const uint8_t *indextransformation);

    uint8_t calculate_checksum(QByteArray output, bool dec_0x100);

    QString parse_message_to_hex(QByteArray received);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_DENSO_SH72543_CAN_DIESEL_OPERATION_H
