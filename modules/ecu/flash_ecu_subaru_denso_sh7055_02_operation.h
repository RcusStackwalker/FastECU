#ifndef FLASH_ECU_SUBARU_DENSO_SH7055_02_OPERATION_H
#define FLASH_ECU_SUBARU_DENSO_SH7055_02_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruDensoSH7055_02 (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
// Owns every serial-> call and the Subaru Denso SH7055 (02) 32-bit K-Line
// bootloader protocol sequence; relocated verbatim from
// FlashEcuSubaruDensoSH7055_02's former private methods.
class FlashEcuSubaruDensoSH7055_02Operation : public FlashOperationWorker
{
    Q_OBJECT

public:
    FlashEcuSubaruDensoSH7055_02Operation(SerialPortActions *serial,
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

    #define KERNEL_MAXSIZE_SUB 8*1024U

    uint32_t CRC32 = 0x5AA5A55A;
    bool crc_tab32_init = 0;
    uint32_t crc_tab32[256];

    bool kernel_alive = false;
    bool test_write = false;
    bool request_denso_kernel_id = false;
    bool flash_write_init = false;

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

    uint32_t flashmsgsize = 0;
    uint32_t flashblocksize = 0;
    uint32_t flashmessagesize = 0;
    uint32_t flashbytescount = 0;
    uint32_t flashbytesindex = 0;

    QString mcu_type_string;
    QString flash_method;
    QString kernel;

    uint8_t bootloader_init_request_fxt02[3] = { 0x4D, 0xFF, 0xB4 };
    uint8_t bootloader_init_response_stock_fxt02_ok[3] = { 0x4D, 0x00, 0xB3 };
    uint8_t bootloader_init_response_ecutek_fxt02_ok[3] = { 0x4D, 0x00, 0xB3 };
    uint8_t bootloader_init_response_cobb_fxt02_ok[3] = { 0x4D, 0x00, 0xB3 };
    uint8_t bootloader_init_response_fxt02_ok[3];

    int connect_bootloader();
    int upload_kernel(QString kernel, uint32_t kernel_start_addr);
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    int get_changed_blocks(const uint8_t *src, int *modified);
    int check_romcrc(const uint8_t *src, uint32_t start, uint32_t len, int *modified);
    int init_flash_write();
    int flash_block(const uint8_t *src, uint32_t start, uint32_t len);
    int reflash_block(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);

    unsigned int crc32(const unsigned char *buf, unsigned int len);
    void init_crc32_tab(void);
    uint8_t cks_add8(QByteArray chksum_data, unsigned len);

    QByteArray send_sid_bf_ssm_init();
    QByteArray send_sid_27_request_seed();
    QByteArray send_sid_27_send_seed_key(QByteArray seed_key);

    QByteArray request_kernel_init();
    QByteArray request_kernel_id();

    QByteArray add_ssm_header(QByteArray output, uint8_t tester_id, uint8_t target_id, bool dec_0x100);
    uint8_t calculate_checksum(QByteArray output, bool dec_0x100);
    int check_received_message(uint8_t *req_response, QByteArray received);
    int connect_bootloader_start_countdown(int timeout);
    QString parse_message_to_hex(QByteArray received);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_DENSO_SH7055_02_OPERATION_H
