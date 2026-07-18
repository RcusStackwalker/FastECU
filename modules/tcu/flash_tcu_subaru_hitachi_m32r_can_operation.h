#ifndef FLASH_TCU_SUBARU_HITACHI_M32R_CAN_OPERATION_H
#define FLASH_TCU_SUBARU_HITACHI_M32R_CAN_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashTcuSubaruHitachiM32rCan (worker-thread migration).
// Owns every serial-> call and the Subaru Hitachi 32bit CAN TCU on-board
// kernel bootloader/read/write sequence; relocated verbatim from
// FlashTcuSubaruHitachiM32rCan's former private methods.
class FlashTcuSubaruHitachiM32rCanOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashTcuSubaruHitachiM32rCanOperation(SerialPortActions *serial,
                                          FileActions::EcuCalDefStructure *ecuCalDef,
                                          QString cmd_type,
                                          QWidget *dialog,
                                          QObject *parent = nullptr,
                                          PromptFn promptOverride = {});

  protected:
    bool execute() override;

  private:
#define STATUS_SUCCESS 0x00
#define STATUS_ERROR 0x01

    bool kernel_alive = false;
    bool test_write = false;
    bool request_denso_kernel_init = false;
    bool request_denso_kernel_id = false;

    int result{};
    int mcu_type_index{};
    int bootloader_start_countdown = 3;

    uint8_t tester_id{};
    uint8_t target_id{};

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
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);
    int erase_mem();

    QByteArray generate_seed_key(const QByteArray& requested_seed);
    QByteArray encrypt_payload(const QByteArray& buf, uint32_t len);
    QByteArray decrypt_payload(const QByteArray& buf, uint32_t len);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_TCU_SUBARU_HITACHI_M32R_CAN_OPERATION_H
