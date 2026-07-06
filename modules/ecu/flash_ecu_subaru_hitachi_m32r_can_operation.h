#ifndef FLASH_ECU_SUBARU_HITACHI_M32R_CAN_OPERATION_H
#define FLASH_ECU_SUBARU_HITACHI_M32R_CAN_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruHitachiM32rCan (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
// Owns every serial-> call and the Subaru Hitachi M32R CAN bootloader
// protocol sequence; relocated verbatim from FlashEcuSubaruHitachiM32rCan's
// former private methods.
class FlashEcuSubaruHitachiM32rCanOperation : public FlashOperationWorker
{
    Q_OBJECT

public:
    FlashEcuSubaruHitachiM32rCanOperation(SerialPortActions *serial,
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

    int connect_bootloader();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);
    int erase_memory();

    QByteArray send_sid_bf_ssm_init();
    QByteArray send_sid_81_start_communication();
    QByteArray send_sid_83_request_timings();
    QByteArray send_sid_27_request_seed();
    QByteArray send_sid_27_send_seed_key(QByteArray seed_key);
    QByteArray send_sid_10_start_diagnostic();
    QByteArray send_sid_34_request_upload(uint32_t dataaddr, uint32_t datalen);
    QByteArray send_sid_36_transferdata(uint32_t dataaddr, QByteArray buf, uint32_t len);
    QByteArray send_sid_31_start_routine();

    QByteArray generate_seed_key(QByteArray requested_seed);
    QByteArray calculate_seed_key(QByteArray requested_seed, const uint16_t *keytogenerateindex, const uint8_t *indextransformation);

    QByteArray encrypt_payload(QByteArray buf, uint32_t len);
    QByteArray decrypt_payload(QByteArray buf, uint32_t len);
    QByteArray calculate_payload(QByteArray buf, uint32_t len, const uint16_t *keytogenerateindex, const uint8_t *indextransformation);

    QByteArray add_ssm_header(QByteArray output, uint8_t tester_id, uint8_t target_id, bool dec_0x100);
    uint8_t calculate_checksum(QByteArray output, bool dec_0x100);

    int connect_bootloader_start_countdown(int timeout);
    QString parse_message_to_hex(QByteArray received);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_HITACHI_M32R_CAN_OPERATION_H
