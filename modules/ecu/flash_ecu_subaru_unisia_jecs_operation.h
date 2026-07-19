#ifndef FLASH_ECU_SUBARU_UNISIA_JECS_OPERATION_H
#define FLASH_ECU_SUBARU_UNISIA_JECS_OPERATION_H

#include <QByteArray>
#include <QString>

#include "src/backend/definitions/kernelcomms.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruUnisiaJecs (worker-thread migration).
// Owns every serial-> call and the Subaru Unisia Jecs SSM-cable read
// sequence; relocated verbatim from FlashEcuSubaruUnisiaJecs's former
// private methods.
class FlashEcuSubaruUnisiaJecsOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruUnisiaJecsOperation(SerialPortActions *serial,
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

    int mcu_type_index{};
    QString mcu_type_string;

    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    int read_mem(uint32_t start_addr, uint32_t length);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_UNISIA_JECS_OPERATION_H
