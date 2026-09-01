#pragma once

#include <QByteArray>
#include <QString>

#include "src/backend/definitions/kernelcomms.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/flash/legacy/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashTcuSubaruDensoSH705xCan (worker-thread migration).
// Owns every serial-> call and the Subaru Denso 07+ 32-bit CAN TCU
// bootloader/flash protocol sequence; relocated verbatim from
// FlashTcuSubaruDensoSH705xCan's former private methods.
class FlashTcuSubaruDensoSH705xCanOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashTcuSubaruDensoSH705xCanOperation(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
                                          QString cmd_type, QWidget *dialog, QObject *parent = nullptr,
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
    int upload_kernel(const QString& kernel, uint32_t kernel_start_addr);
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    int get_changed_blocks(const uint8_t *src, int *modified);
    int check_romcrc(const uint8_t *src, uint32_t addr, uint32_t len, int *modified);
    int init_flash_write();
    int flash_block(const uint8_t *src, uint32_t start, uint32_t len);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);

    QByteArray generate_seed_key(const QByteArray& requested_seed);
    QByteArray generate_ecutek_seed_key(QByteArray requested_seed);
    QByteArray generate_cobb_seed_key(QByteArray requested_seed);

    QByteArray request_kernel_init();
    QByteArray request_kernel_id();

    QByteArray encrypt_payload(const QByteArray& buf, uint32_t len);
    QByteArray decrypt_payload(const QByteArray& buf, uint32_t len);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};
