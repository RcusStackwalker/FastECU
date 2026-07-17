#ifndef FLASH_ECU_SUBARU_DENSO_MC68HC16Y5_02_BDM_OPERATION_H
#define FLASH_ECU_SUBARU_DENSO_MC68HC16Y5_02_BDM_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruDensoMC68HC16Y5_02_BDM (worker-thread migration).
// Owns every serial-> call and the Subaru Denso MC68HC16 BDM unbrick
// sequence; relocated verbatim from
// FlashEcuSubaruDensoMC68HC16Y5_02_BDM's former private methods.
class FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation(SerialPortActions *serial,
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

    int result{};
    int mcu_type_index{};

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

    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem();
    int flash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_DENSO_MC68HC16Y5_02_BDM_OPERATION_H
