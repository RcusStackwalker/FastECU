#ifndef FLASH_ECU_SUBARU_HITACHI_M32R_JTAG_OPERATION_H
#define FLASH_ECU_SUBARU_HITACHI_M32R_JTAG_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruHitachiM32rJtag (worker-thread migration).
// Owns every serial-> call and the Subaru Hitachi M32R JTAG probing
// sequence; relocated verbatim from
// FlashEcuSubaruHitachiM32rJtag's former private methods.
class FlashEcuSubaruHitachiM32rJtagOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruHitachiM32rJtagOperation(SerialPortActions *serial,
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

    uint8_t inst_tool_rom_code[16] = {0xd1, 0xc0, 0xff, 0x00,
                                      0xa0, 0xc1, 0x3f, 0xfc,
                                      0x20, 0x44, 0xf0, 0x00,
                                      0x7f, 0xf4, 0xf0, 0x00};

    uint8_t id_code = 0x02;

    QString IDCODE = "02";
    QString USERCODE = "03";
    QString MDM_SYSTEM = "08";
    QString MDM_CONTROL = "09";
    QString MDM_SETUP = "0a";
    QString MTM_CONTROL = "0f";
    QString MON_CODE = "10";
    QString MON_DATA = "11";
    QString MON_PARAM = "12";
    QString MON_ACCESS = "13";
    QString DMA_RADDR = "18";
    QString DMA_RDATA = "19";
    QString DMA_RTYPE = "1a";
    QString DMA_ACCESS = "1b";
    QString RTDENB = "20";

    bool kernel_alive = false;
    bool test_write = false;

    int result;
    int mcu_type_index;
    int bootloader_start_countdown = 3;

    uint8_t comm_try_timeout = 50;
    uint8_t comm_try_count = 4;

    uint8_t tester_id;
    uint8_t target_id;

    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    uint16_t jtag_timeout = 5;
    uint16_t jtag_loopcount_max = 2000;

    uint32_t flashbytescount = 0;
    uint32_t flashbytesindex = 0;

    QString mcu_type_string;
    QString flash_method;
    QString kernel;

    int init_jtag();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);

    void hard_reset_jtag();
    int read_idcode();
    int read_usercode();
    void set_rtdenb();
    int read_tool_rom_code();

    void write_jtag_ir(QString desc, QString code);
    QByteArray read_jtag_dr(QString desc);
    QByteArray write_jtag_dr(QString desc, QString data);
    QByteArray read_response();

    QByteArray add_header(QByteArray output);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_HITACHI_M32R_JTAG_OPERATION_H
