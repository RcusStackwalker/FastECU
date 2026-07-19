#ifndef FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_OPERATION_H
#define FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_OPERATION_H

#include <QByteArray>
#include <QString>

#include "src/backend/definitions/kernelcomms.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruDensoSH7058CanDiesel (worker-thread migration).
// Owns every serial-> call and the Subaru 07+ Diesel 32-bit CAN (iso15765)
// bootloader protocol sequence; relocated verbatim from
// FlashEcuSubaruDensoSH7058CanDiesel's former private methods.
class FlashEcuSubaruDensoSH7058CanDieselOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruDensoSH7058CanDieselOperation(SerialPortActions *serial,
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

#define CRC32 0x5AA5A55A

    bool kernel_alive = false;
    bool test_write = false;
    bool request_denso_kernel_id = false;
    bool flash_write_init = false;

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

    uint32_t flashmessagesize = 0;
    uint32_t flashblocksize = 0;
    uint32_t flashbytescount = 0;
    uint32_t flashbytesindex = 0;

    QString mcu_type_string;
    QString flash_method;
    QString kernel;

    int connect_bootloader();
    int upload_kernel(const QString& kernel, uint32_t kernel_addr);
    int read_mem(uint32_t addr, uint32_t length);
    int write_mem(bool test_write);
    int get_changed_blocks(const uint8_t *src, int *modified);
    int check_romcrc(const uint8_t *src, uint32_t addr, uint32_t len, int *modified);
    int init_flash_write();
    int flash_block(const uint8_t *src, uint32_t addr, uint32_t len);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);

    QByteArray generate_seed_key(const QByteArray& requested_seed);
    QByteArray generate_ecutek_seed_key(const QByteArray& requested_seed);
    QByteArray generate_cobb_seed_key(const QByteArray& requested_seed);

    QByteArray request_kernel_id();

    QByteArray encrypt_payload(const QByteArray& buf, uint32_t len);
    QByteArray decrypt_payload(const QByteArray& buf, uint32_t len);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_DENSO_SH7058_CAN_DIESEL_OPERATION_H
