#ifndef FLASH_TCU_SUBARU_HITACHI_M32R_KLINE_OPERATION_H
#define FLASH_TCU_SUBARU_HITACHI_M32R_KLINE_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashTcuSubaruHitachiM32rKline (worker-thread migration).
// Owns every serial-> call and the Subaru Hitachi 32bit K-Line TCU
// bootloader/read sequence; relocated verbatim from
// FlashTcuSubaruHitachiM32rKline's former private methods.
class FlashTcuSubaruHitachiM32rKlineOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashTcuSubaruHitachiM32rKlineOperation(SerialPortActions *serial,
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
    int read_a0_rom(uint32_t start_addr, uint32_t length);
    int read_a0_ram(uint32_t start_addr, uint32_t length);
    int read_b8(uint32_t start_addr, uint32_t length);
    int read_b0(uint32_t start_addr, uint32_t length);

    QByteArray encrypt_payload();

    QByteArray send_sid_a0_block_read(uint32_t dataaddr, uint32_t datalen);
    QByteArray send_sid_b8_byte_read(uint32_t dataaddr);
    QByteArray send_sid_b0_block_write(uint32_t dataaddr, uint32_t datalen);

    QByteArray generate_kline_seed_key(QByteArray seed);
    QByteArray generate_can_seed_key(QByteArray requested_seed);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_TCU_SUBARU_HITACHI_M32R_KLINE_OPERATION_H
