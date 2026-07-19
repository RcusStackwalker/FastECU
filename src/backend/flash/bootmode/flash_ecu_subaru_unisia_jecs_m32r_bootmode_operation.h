#ifndef FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOTMODE_OPERATION_H
#define FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOTMODE_OPERATION_H

#include <QByteArray>
#include <QString>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruUnisiaJecsM32rBootMode (worker-thread migration).
// Owns every serial-> call and the Subaru Unisia Jecs UJ20/30 boot-mode
// K-Line protocol sequence; relocated verbatim from
// FlashEcuSubaruUnisiaJecsM32rBootMode's former private methods.
class FlashEcuSubaruUnisiaJecsM32rBootModeOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruUnisiaJecsM32rBootModeOperation(SerialPortActions *serial,
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

#define SID_UNISIA_JECS_BLOCK_READ 0xA0
#define SID_UNISIA_JECS_ADDR_READ 0xA8
#define SID_UNISIA_JECS_BLOCK_WRITE 0xB0
#define SID_UNISIA_JECS_ADDR_WRITE 0xB8
#define SID_UNISIA_JECS_FLASH_READ 0x00 //???

#define SID_UNISIA_JECS_FLASH_ERASE 0x31 //???
#define SID_UNISIA_JECS_FLASH_WRITE 0x61
#define SID_UNISIA_JECS_FLASH_WRITE_END 0x69 //???

    bool kernel_alive = false;
    bool test_write = false;
    int result{};
    int mcu_type_index{};
    int bootloader_start_countdown = 3;

    uint8_t tester_id{};
    uint8_t target_id{};

    uint8_t comm_try_timeout = 50;
    uint8_t comm_try_count = 4;

    uint16_t receive_timeout = 500;
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

    int upload_kernel(const QString& kernel);
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem();

    QByteArray send_sid_bf_ssm_init();
    QByteArray send_subaru_sid_b8_change_baudrate_4800();
    QByteArray send_subaru_sid_b8_change_baudrate_38400();

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_UNISIA_JECS_M32R_BOOTMODE_OPERATION_H
