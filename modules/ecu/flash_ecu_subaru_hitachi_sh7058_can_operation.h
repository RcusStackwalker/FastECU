#ifndef FLASH_ECU_SUBARU_HITACHI_SH7058_CAN_OPERATION_H
#define FLASH_ECU_SUBARU_HITACHI_SH7058_CAN_OPERATION_H

#include <QByteArray>
#include <QString>

#include <kernelcomms.h>
#include <kernelmemorymodels.h>
#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuSubaruHitachiSH7058Can (worker-thread migration).
// Owns every serial-> call and the Subaru Hitachi SH7058 CAN bootloader
// protocol sequence; relocated verbatim from
// FlashEcuSubaruHitachiSH7058Can's former private methods.
class FlashEcuSubaruHitachiSH7058CanOperation : public FlashOperationWorker
{
    Q_OBJECT

  public:
    FlashEcuSubaruHitachiSH7058CanOperation(SerialPortActions *serial,
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

    int connect_bootloader_subaru_ecu_hitachi_can();
    // int upload_kernel_subaru_denso_can_02_32bit(QString kernel, uint32_t kernel_start_addr);
    int read_mem_subaru_ecu_hitachi_can(uint32_t start_addr, uint32_t length);
    int write_mem_subaru_ecu_hitachi_can(bool test_write);
    // int get_changed_blocks_denso_can_02_32bit(const uint8_t *src, int *modified);
    // int check_romcrc_denso_can_02_32bit(const uint8_t *src, uint32_t start_addr, uint32_t len, int *modified);
    // int flash_block_denso_can_02_32bit(const uint8_t *src, uint32_t start, uint32_t len);
    int reflash_block_subaru_ecu_hitachi_can(const uint8_t *newdata, const struct flashdev_t *fdt, unsigned blockno, bool test_write);
    int erase_subaru_ecu_hitachi_can();

    QByteArray send_subaru_sid_10_start_diagnostic();
    QByteArray send_sid_bf_ssm_init();
    QByteArray send_subaru_sid_b8_change_baudrate_38400();
    QByteArray send_subaru_sid_81_start_communication();
    QByteArray send_subaru_sid_83_request_timings();

    // uint8_t cks_add8(QByteArray chksum_data, unsigned len);
    // void init_crc16_tab(void);
    // uint16_t crc16(const uint8_t *data, uint32_t siz);

    // QByteArray send_subaru_ecu_sid_bf_ssm_init();
    // QByteArray send_subaru_ecu_sid_a0_block_read(uint32_t dataaddr, uint32_t datalen);
    // QByteArray send_sid_81_start_communication();
    // QByteArray send_sid_83_request_timings();
    // QByteArray send_sid_27_request_seed();
    // QByteArray send_sid_27_send_seed_key(QByteArray seed_key);
    // QByteArray send_sid_10_start_diagnostic();
    // QByteArray send_sid_34_request_upload(uint32_t dataaddr, uint32_t datalen);
    // QByteArray send_sid_36_transferdata(uint32_t dataaddr, QByteArray buf, uint32_t len);
    // QByteArray send_sid_31_start_routine();

    // QByteArray request_kernel_init();
    // QByteArray request_kernel_id();

    QByteArray subaru_ecu_hitachi_generate_can_seed_key(const QByteArray& requested_seed);
    QByteArray subaru_ecu_hitachi_encrypt_32bit_payload(const QByteArray& buf, uint32_t len);
    QByteArray subaru_ecu_hitachi_decrypt_32bit_payload(const QByteArray& buf, uint32_t len);

    // int connect_bootloader_start_countdown(int timeout);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
};

#endif // FLASH_ECU_SUBARU_HITACHI_SH7058_CAN_OPERATION_H
