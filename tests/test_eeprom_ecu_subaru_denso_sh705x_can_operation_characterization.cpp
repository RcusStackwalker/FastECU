// Characterization tests for EepromEcuSubaruDensoSH705xCanOperation
// (src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.cpp).
//
// Purpose (step 5c, Task 7): this legacy ~1,406-line QThread-based operation
// class has no focused tests today and is slated for deletion once Task 9's
// portable DensoSh705xEepromCanExecutor replaces it. Every literal byte
// sequence built by the helpers below is transcribed from a specific line
// range in the real .cpp (cited in each helper's comment) -- nothing here is
// invented. Where a value is runtime-computed in production (the seed key,
// the encrypted kernel payload), the test calls the exact same SsmProtocol
// helper (or, for the RSA-based EcuTek RaceRom variant and the
// _ecutek_racerom_alt XOR/multiply step, replicates the exact inline
// arithmetic) with the exact same tables/constants the production code uses,
// rather than hardcoding a value that could not be independently verified.
//
// Structural note vs. the K-Line sibling
// (test_eeprom_ecu_subaru_denso_sh705x_kline_operation_characterization.cpp):
// the CAN class's header declares nine send_sid_*() helper methods plus
// request_kernel_init() / calculate_ecutek_racerom_seed_key() /
// init_crc16_tab() / crc16(), but NONE of them are defined anywhere in the
// .cpp -- connect_bootloader()/upload_kernel()/read_mem() build every UDS-
// over-CAN frame inline instead. Those nine+ declarations are dead surface
// left over from an earlier refactor; harmless (never ODR-used, so the
// class still links), but worth flagging for whoever reads the header
// expecting them to be the real call sites.
//
// Harness pattern follows tests/test_flash_ecu_mitsu_m32r_can_operation.cpp
// and the K-Line sibling above: a real SerialPortActions constructed with
// the backendFactoryForTests hook returning a FakeBackend/QueuedFakeBackend,
// the real operation class driven via QThread::start(), and exact byte-
// sequence assertions via fake->takeCallLog().

#include "src/backend/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QMessageBox>
#include <QQueue>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryFile>
#include <QTest>
#include <QWidget>

#include "fake_backend.h"
#include "src/algorithms/protocol/ssm/ssm_protocol.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/definitions/kernelcomms.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "test_eeprom_ecu_subaru_denso_sh705x_can_operation_characterization.h"

class QueuedFakeBackend : public FakeBackend
{
    Q_OBJECT
  public:
    QQueue<QByteArray> responses;

    QByteArray read_serial_data(uint16_t timeout) override
    {
        Q_UNUSED(timeout)
        if (responses.isEmpty())
        {
            return QByteArray();
        }
        return responses.dequeue();
    }
};

namespace
{

QString hex(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex());
}

// ---- Request builders: TRANSCRIBE of every inline UDS-over-CAN frame -----
//
// Every non-read_ram_location() request below is built inline in
// connect_bootloader()/upload_kernel()/read_mem() with a hardcoded 4-byte
// "CAN ID" prefix [0x00, 0x00, 0x07, 0xE0] -- there is no addHeader() call
// site anywhere in this class for these frames (contrast read_ram_location(),
// transcribed separately below, which *is* SsmProtocol::addHeader()-framed).

// connect_bootloader(), lines 238-246: output = [0x00,0x00,0x07,0xE0,0x01,0x00].
QByteArray initConnectionRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x01));
    out.append(char(0x00));
    return out;
}

// connect_bootloader(), lines 274-281: output = [0x00,0x00,0x07,0xE0,0xAA].
QByteArray ecuIdRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0xAA));
    return out;
}

// connect_bootloader(), lines 315-322: output = [0x00,0x00,0x07,0xE0,0x09,0x02].
QByteArray vinRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x09));
    out.append(char(0x02));
    return out;
}

// connect_bootloader(), lines 346-353: output = [0x00,0x00,0x07,0xE0,0x09,0x04].
QByteArray calIdRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x09));
    out.append(char(0x04));
    return out;
}

// connect_bootloader(), lines 381-388: output = [0x00,0x00,0x07,0xE0,0x09,0x06].
QByteArray cvnRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x09));
    out.append(char(0x06));
    return out;
}

// connect_bootloader(), lines 420-428: output = [0x00,0x00,0x07,0xE0,0x10,0x03].
QByteArray sessionMode03Request()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x10));
    out.append(char(0x03));
    return out;
}

// connect_bootloader(), lines 448-455: output = [0x00,0x00,0x07,0xE0,0x10,0x43].
QByteArray sessionMode43Request()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x10));
    out.append(char(0x43));
    return out;
}

// connect_bootloader(), lines 475-483: output = [0x00,0x00,0x07,0xE0,0x27,0x01].
QByteArray seedRequestFrame()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x27));
    out.append(char(0x01));
    return out;
}

// connect_bootloader(), lines 543-553: output = [0x00,0x00,0x07,0xE0,0x27,0x02] + seed_key (4 bytes).
// Branch selecting which generate_*_seed_key() produces seed_key: lines 522-541.
QByteArray seedKeySendRequest(const QByteArray& key)
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x27));
    out.append(char(0x02));
    out.append(key);
    return out;
}

// connect_bootloader(), lines 586-602: output = [0x00,0x00,0x07,0xE0,0x10] with
// 0x02 appended iff req_10_03_connected, then 0x42 appended iff
// req_10_43_connected. Every fixture in this file gives positive responses to
// both prior session-mode requests, so both flags are true and both bytes
// are appended (7-byte frame) -- this helper only covers that case.
QByteArray sessionSetRequestBothConnected()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x10));
    out.append(char(0x02));
    out.append(char(0x42));
    return out;
}

// request_kernel_id(), lines 1355-1390: output = [0x00,0x00,0x07,0xE0,
// (SUB_KERNEL_START_COMM>>8)&0xFF, SUB_KERNEL_START_COMM&0xFF,
// ((datalen+1)>>8)&0xFF, (datalen+1)&0xFF, SUB_KERNEL_ID&0xFF, 0x00,0x00,0x00]
// with datalen==0 hardcoded (line 1364). UNLIKE the K-Line sibling's
// request_kernel_id(), this one is NOT checksum-terminated -- no
// SsmProtocol::checksum() call exists in this function at all; the 12 bytes
// below are everything that gets written.
QByteArray requestKernelIdRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    out.append(char(SUB_KERNEL_START_COMM & 0xFF));
    out.append(char(((0 + 1) >> 8) & 0xFF));
    out.append(char((0 + 1) & 0xFF));
    out.append(char(SUB_KERNEL_ID & 0xFF));
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x00));
    return out;
}

// upload_kernel(), lines 765-780: output = [0x00,0x00,0x07,0xE0, 0x34,0x04,0x33,
// start_address>>16, start_address>>8, start_address, data_len>>16,
// data_len>>8, data_len].
QByteArray sid34RequestDownloadRequest(quint32 startAddress, quint32 dataLen)
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x34));
    out.append(char(0x04));
    out.append(char(0x33));
    out.append(char((startAddress >> 16) & 0xFF));
    out.append(char((startAddress >> 8) & 0xFF));
    out.append(char(startAddress & 0xFF));
    out.append(char((dataLen >> 16) & 0xFF));
    out.append(char((dataLen >> 8) & 0xFF));
    out.append(char(dataLen & 0xFF));
    return out;
}

// upload_kernel(), lines 803-852 (loop at 816-857): per-block output =
// [0x00,0x00,0x07,0xE0, 0xB6, blockaddr>>16, blockaddr>>8, blockaddr] + up to
// 128 bytes of already-encrypted payload. The loop runs blockno = 0..maxblocks
// INCLUSIVE (maxblocks+1 iterations): blocks 0..maxblocks-1 each send a full
// 128-byte chunk and decrement a running `data_len` by 128; the final
// iteration (blockno == maxblocks) sends whatever remains of that running
// `data_len`. Since data_len is initialized to exactly maxblocks*128, it has
// been decremented to exactly 0 by the time that final iteration runs -- so
// the *last* 0xB6 frame this loop sends is always header-only, zero payload
// bytes. This is a real, easy-to-miss behavior: uploading an N-block kernel
// produces N+1 wire frames, the last one empty.
QByteArray sidB6TransferBlockRequest(quint32 blockAddr, const QByteArray& payload)
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0xB6));
    out.append(char((blockAddr >> 16) & 0xFF));
    out.append(char((blockAddr >> 8) & 0xFF));
    out.append(char(blockAddr & 0xFF));
    out.append(payload);
    return out;
}

// upload_kernel(), lines 862-869: output = [0x00,0x00,0x07,0xE0,0x37].
QByteArray sid37StartKernelRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x37));
    return out;
}

// upload_kernel(), lines 894-905: output = [0x00,0x00,0x07,0xE0,0x31,0x01,0x02,0x02,0x02].
QByteArray sid31StartRoutineRequest()
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xE0));
    out.append(char(0x31));
    out.append(char(0x01));
    out.append(char(0x02));
    out.append(char(0x02));
    out.append(char(0x02));
    return out;
}

// read_mem(), lines 987-1004 (template) + 1029-1033 (per-iteration overwrite
// of output[10..14]), lines 993-997: output = [0x00,0x00,0x07,0xe0,
// (SUB_KERNEL_START_COMM>>8)&0xFF, SUB_KERNEL_START_COMM&0xFF,
// ((datalen+1)>>8)&0xFF, (datalen+1)&0xFF, SUB_KERNEL_READ_EEPROM,
// EEPROM_MODE, addr>>16, addr>>8, addr, pagesize>>8, pagesize] with
// datalen==6 hardcoded (line 973). For McuType "SH7055"
// (flashdevices[].eblocks[0] == {start=0, len=0x100},
// src/backend/definitions/kernelmemorymodels.h:279-281,540), read_mem()'s
// pagesize/skip_start/willget arithmetic (lines 975-982) reduces to a single
// request with addr=0, pagesize=0x100 (256), regardless of EEPROM_MODE.
QByteArray sidReadEepromRequestForSh7055(quint8 eepromMode)
{
    QByteArray out;
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x07));
    out.append(char(0xe0));
    out.append(char((SUB_KERNEL_START_COMM >> 8) & 0xFF));
    out.append(char(SUB_KERNEL_START_COMM & 0xFF));
    out.append(char(((6 + 1) >> 8) & 0xFF));
    out.append(char((6 + 1) & 0xFF));
    out.append(char(SUB_KERNEL_READ_EEPROM));
    out.append(char(eepromMode));
    out.append(char(0x00)); // addr>>16 (addr == 0)
    out.append(char(0x00)); // addr>>8
    out.append(char(0x00)); // addr
    out.append(char(0x01)); // pagesize>>8 (pagesize == 0x100)
    out.append(char(0x00)); // pagesize
    return out;
}

// The fixed 9-byte prefix shared by every sidReadEepromRequestForSh7055(...)
// frame regardless of EEPROM_MODE (which is byte index 9) -- used to filter
// the call log for "was a READ_EEPROM request sent" without depending on
// mode, anchored so it can't false-positive against an unrelated request
// (see the K-Line sibling's report for why unanchored substring filters on
// hex payloads are fragile).
QString sidReadEepromHexPrefix()
{
    QByteArray prefix;
    prefix.append(char(0x00));
    prefix.append(char(0x00));
    prefix.append(char(0x07));
    prefix.append(char(0xe0));
    prefix.append(char(0xBE));
    prefix.append(char(0xEF));
    prefix.append(char(0x00));
    prefix.append(char(0x07));
    prefix.append(char(SUB_KERNEL_READ_EEPROM));
    return hex(prefix);
}

// read_ram_location(), lines 637-694 -- ONLY reached from
// connect_bootloader()'s "_ecutek_racerom_alt" branch (lines 195-234), and
// ONLY after that branch has already called
// serial->set_is_iso15765_connection(false) (line 200). Unlike every request
// above, this one IS addHeader()-framed (lines 663-666: `if
// (!serial->get_is_iso15765_connection()) output =
// SsmProtocol::addHeader(output, tester_id, target_id, false);`) and has NO
// [0x00,0x00,0x07,0xE0] CAN-ID prefix, since that prefix is only prepended
// when get_is_iso15765_connection() is true (lines 646-652) -- the opposite
// of the state this call site runs in. tester_id/target_id are private
// uint8_t members default-initialized to 0 (header line 48-49) and never
// assigned anywhere in this class, so the frame is addHeader(output, 0, 0,
// false). Pre-header output = [0xA8, 0x00, then 4x (loc>>16, loc>>8, loc),
// loc incrementing by 1 after each 3-byte group] (lines 654-662) -- 14 bytes.
//
// This exact wire format (SSM 0x80-style framing, no CAN prefix) is itself
// the observable proof that is_iso15765_connection() was false at the moment
// this request was built -- i.e. that connect_bootloader() really did
// reconfigure to K-Line-like settings before issuing it.
QByteArray ramReadRequest(quint32 loc)
{
    QByteArray out;
    out.append(char(0xA8));
    out.append(char(0x00));
    for (int i = 0; i < 4; ++i)
    {
        out.append(char((loc >> 16) & 0xFF));
        out.append(char((loc >> 8) & 0xFF));
        out.append(char(loc & 0xFF));
        ++loc;
    }
    return SsmProtocol::addHeader(out, 0, 0, false);
}

// ---- Seed-key algorithms: TRANSCRIBE of each generate_*_seed_key() --------

// generate_seed_key(), lines 1146-1172 (the stock/default branch: flash_method
// doesn't end with any of "_ecutek_racerom_alt"/"_ecutek_racerom"/"_ecutek"/
// "_cobb").
QByteArray generateSeedKeyStock(const QByteArray& seed)
{
    const uint16_t keytogenerateindex_1[] = {
        0x78B1, 0x4625, 0x201C, 0x9EA5,
        0xAD6B, 0x35F4, 0xFD21, 0x5E71,
        0xB046, 0x7F4A, 0x4B75, 0x93F9,
        0x1895, 0x8961, 0x3ECC, 0x862B};
    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, keytogenerateindex_1, indextransformation);
}

// generate_ecutek_seed_key(), lines 1224-1269, the base calculateSeedKey()
// call only (lines 1228-1247); the "_ecutek_racerom_alt" post-processing
// (lines 1249-1266) is transcribed separately below as
// generateEcutekRaceromAltSeedKey() since it needs seed_alter/xor_byte_1/
// xor_byte_2, which are only ever set by the RAM-read branch. Note
// keytogenerateindex_1 here is IDENTICAL to the stock table above; only
// indextransformation's first 5 entries differ (0x4,0x2,0x5,0x1,0x8 vs
// stock's 0x5,0x6,0x7,0x1,0x9).
QByteArray generateEcutekSeedKeyPlain(const QByteArray& seed)
{
    const uint16_t keytogenerateindex_1[] = {
        0x78B1, 0x4625, 0x201C, 0x9EA5,
        0xAD6B, 0x35F4, 0xFD21, 0x5E71,
        0xB046, 0x7F4A, 0x4B75, 0x93F9,
        0x1895, 0x8961, 0x3ECC, 0x862B};
    const uint8_t indextransformation[] = {
        0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, keytogenerateindex_1, indextransformation);
}

// generate_ecutek_seed_key()'s "_ecutek_racerom_alt" special case, lines
// 1249-1266: re-derives the 4-byte key from the plain EcuTek key plus
// seed_alter/xor_byte_1/xor_byte_2 (populated by the two read_ram_location()
// calls) and the class's xor_multi constant (header line 63:
// `uint32_t xor_multi = 0x01000193;`).
QByteArray generateEcutekRaceromAltSeedKey(const QByteArray& seed, quint32 seedAlter, quint8 xorByte1, quint8 xorByte2)
{
    const QByteArray base = generateEcutekSeedKeyPlain(seed);
    quint32 etRrSeed = (quint32(quint8(base.at(0))) << 24) & 0xFF000000u;
    etRrSeed += (quint32(quint8(base.at(1))) << 16) & 0x00FF0000u;
    etRrSeed += (quint32(quint8(base.at(2))) << 8) & 0x0000FF00u;
    etRrSeed += quint32(quint8(base.at(3))) & 0x000000FFu;

    constexpr quint32 kXorMulti = 0x01000193u;
    etRrSeed = ((seedAlter ^ etRrSeed) ^ xorByte1) * kXorMulti;
    etRrSeed = (etRrSeed ^ xorByte2) * kXorMulti;

    QByteArray out;
    out.append(char((etRrSeed >> 24) & 0xff));
    out.append(char((etRrSeed >> 16) & 0xff));
    out.append(char((etRrSeed >> 8) & 0xff));
    out.append(char(etRrSeed & 0xff));
    return out;
}

// generate_cobb_seed_key(), lines 1274-1302 ("2017 VA model" table, the one
// actually passed to calculateSeedKey() -- the "2012 STi" keytogenerateindex_2
// table alongside it is declared but never used by calculateSeedKey() here,
// same dead-alternate-table shape as the stock/ecutek functions' unused
// keytogenerateindex_2).
QByteArray generateCobbSeedKey(const QByteArray& seed)
{
    const uint16_t keytogenerateindex_1[] = {
        0x9DDB, 0x9CFB, 0x9B9A, 0x6136,
        0x59E1, 0xBA03, 0xD683, 0x7092,
        0x9E05, 0x8723, 0xF998, 0x15BB,
        0xB8D5, 0xFF0C, 0x9D91, 0x24B9};
    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, keytogenerateindex_1, indextransformation);
}

// decrypt_racerom_seed(), lines 1175-1189: plain modular exponentiation
// (square-and-multiply), replicated verbatim rather than reimplemented via
// SsmProtocol (this algorithm has no SsmProtocol equivalent -- it's specific
// to this class).
unsigned long long decryptRaceromSeed(unsigned long long base, unsigned long long exponent, unsigned long long modulus)
{
    unsigned long long result = 1;
    base = base % modulus;
    while (exponent > 0)
    {
        if (exponent & 1)
        {
            result = (result * 1LL * base) % modulus;
        }
        base = (base * 1LL * base) % modulus;
        exponent = exponent / 2;
    }
    return result;
}

// generate_ecutek_racerom_can_seed_key(), lines 1191-1217: seed bytes packed
// big-endian into a uint32, RSA-decrypted with the hardcoded d=0x0A863281,
// n=0x0fda9293, then re-emitted big-endian as the 4-byte key.
QByteArray generateEcutekRacecomCanSeedKey(const QByteArray& seed)
{
    quint32 seedWord = (quint32(quint8(seed.at(0))) << 24) & 0xFF000000u;
    seedWord += (quint32(quint8(seed.at(1))) << 16) & 0x00FF0000u;
    seedWord += (quint32(quint8(seed.at(2))) << 8) & 0x0000FF00u;
    seedWord += quint32(quint8(seed.at(3))) & 0x000000FFu;

    constexpr unsigned long long d = 0x0A863281ULL;
    constexpr unsigned long long n = 0x0fda9293ULL;
    const quint32 decrypted = quint32(decryptRaceromSeed(seedWord, d, n));

    QByteArray out;
    out.append(char((decrypted >> 24) & 0xFF));
    out.append(char((decrypted >> 16) & 0xFF));
    out.append(char((decrypted >> 8) & 0xFF));
    out.append(char(decrypted & 0xFF));
    return out;
}

// encrypt_payload(), lines 1314-1330: this class's OWN key table --
// {0xC85B, 0x32C0, 0xE282, 0x92A0} -- distinct from the K-Line sibling's
// encrypt_payload() table ({0x7856, 0xCE22, 0xF513, 0x6E86}); the shared
// indextransformation table is the same 32-entry constant used everywhere
// else in this class.
QByteArray encryptPayloadCan(const QByteArray& buf, uint32_t len)
{
    const uint16_t keytogenerateindex[] = {0xC85B, 0x32C0, 0xE282, 0x92A0};
    const uint8_t indextransformation[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
        0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
        0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, keytogenerateindex, indextransformation);
}

// ---- Kernel-upload framing: TRANSCRIBE of upload_kernel()'s padding/------
// ---- checksum/encrypt pipeline (lines 701-762) and B6 chunking (803-857) --

struct KernelUploadPlan
{
    quint32 dataLen = 0;
    quint16 maxBlocks = 0;
    QByteArray encryptedPayload; // length == dataLen
};

// Mirrors upload_kernel() lines 734-760 exactly:
//   pl_len = (file_len + 3) & ~3            (rounded UP to a multiple of 4)
//   maxblocks = pl_len/128 (+1 if remainder)
//   end_addr = (start_address + maxblocks*128) & 0xFFFFFFFF
//   data_len = end_addr - start_address     ( == maxblocks*128, see below)
//   pad pl_encr with 0x00 up to data_len, drop the last 4 bytes, append a
//   32-bit-word checksum (0x5aa5a55a - sum of all big-endian 32-bit words),
//   then encrypt the whole (now data_len-byte) buffer.
// data_len depends only on maxblocks*128 and start_address's low bits, which
// don't overflow for any address/kernel size this test uses, so this helper
// omits start_address (unlike production, which threads it through
// end_addr's wraparound mask defensively).
KernelUploadPlan computeKernelUploadPlan(const QByteArray& kernelBytes)
{
    KernelUploadPlan plan;
    const quint32 fileLen = quint32(kernelBytes.size());
    const quint32 plLen = (fileLen + 3) & ~3u;
    QByteArray plEncr = kernelBytes;

    plan.maxBlocks = quint16(plLen / 128);
    if ((plLen % 128) != 0)
    {
        plan.maxBlocks++;
    }
    plan.dataLen = quint32(plan.maxBlocks) * 128;

    while (quint32(plEncr.size()) < plan.dataLen)
    {
        plEncr.append(char(0x00));
    }
    plEncr.remove(plEncr.size() - 4, 4);

    quint32 chkSum = 0;
    for (int i = 0; i < plEncr.size(); i += 4)
    {
        chkSum += (quint32(quint8(plEncr.at(i))) << 24) | (quint32(quint8(plEncr.at(i + 1))) << 16) |
                  (quint32(quint8(plEncr.at(i + 2))) << 8) | quint32(quint8(plEncr.at(i + 3)));
    }
    chkSum = 0x5aa5a55au - chkSum;

    plEncr.append(char((chkSum >> 24) & 0xFF));
    plEncr.append(char((chkSum >> 16) & 0xFF));
    plEncr.append(char((chkSum >> 8) & 0xFF));
    plEncr.append(char(chkSum & 0xFF));

    plan.encryptedPayload = encryptPayloadCan(plEncr, quint32(plEncr.size()));
    return plan;
}

// Computes the exact ordered write sequence upload_kernel() produces for a
// given kernel fixture: SID34 request-download, one 0xB6 frame per block
// (the last one empty, see sidB6TransferBlockRequest()'s comment), 0x37,
// 0x31, then the post-upload request_kernel_id() poll.
QStringList expectedUploadKernelWrites(const QByteArray& kernelBytes, quint32 startAddress)
{
    QStringList out;
    auto add = [&out](const QByteArray& request)
    { out << "write_echo_check:begin:" + hex(request); };

    const KernelUploadPlan plan = computeKernelUploadPlan(kernelBytes);
    add(sid34RequestDownloadRequest(startAddress, plan.dataLen));

    quint32 remaining = plan.dataLen;
    for (int blockno = 0; blockno <= plan.maxBlocks; ++blockno)
    {
        const quint32 blockAddr = startAddress + quint32(blockno) * 128;
        QByteArray payload;
        if (blockno == plan.maxBlocks)
        {
            payload = plan.encryptedPayload.mid(blockno * 128, int(remaining));
        }
        else
        {
            payload = plan.encryptedPayload.mid(blockno * 128, 128);
            remaining -= 128;
        }
        add(sidB6TransferBlockRequest(blockAddr, payload));
    }

    add(sid37StartKernelRequest());
    add(sid31StartRoutineRequest());
    add(requestKernelIdRequest());
    return out;
}

// Computes the exact ordered write sequence a full (kernel-not-yet-running)
// connect_bootloader() round produces, given the already-selected seed key.
QStringList expectedConnectBootloaderFullInitWrites(const QByteArray& key)
{
    QStringList out;
    auto add = [&out](const QByteArray& request)
    { out << "write_echo_check:begin:" + hex(request); };

    add(requestKernelIdRequest());
    add(initConnectionRequest());
    add(ecuIdRequest());
    add(vinRequest());
    add(calIdRequest());
    add(cvnRequest());
    add(sessionMode03Request());
    add(sessionMode43Request());
    add(seedRequestFrame());
    add(seedKeySendRequest(key));
    add(sessionSetRequestBothConnected());
    return out;
}

// ---- Response fixtures ----------------------------------------------------

// request_kernel_id()'s "kernel not running" response: production only
// branches into the "kernel alive" fast path when received.length() > 8
// (connect_bootloader() line 174 / upload_kernel() line 932); an empty
// response falls into the else branch, logs an error, and falls through to
// full re-init without returning early.
QByteArray kernelNotAliveResponse()
{
    return QByteArray();
}

// request_kernel_id()'s "kernel alive" response, checked identically at
// connect_bootloader() lines 174-187 and upload_kernel()'s poll lines
// 930-946: received[4..5] == SUB_KERNEL_START_COMM big-endian (0xBE, 0xEF),
// received[8] == SUB_KERNEL_ID | 0x40 == 0x41. Trailing "kernel id" bytes are
// logged but never asserted.
QByteArray kernelAliveResponse()
{
    QByteArray out(9, char(0));
    out[4] = char(0xBE);
    out[5] = char(0xEF);
    out[8] = char(SUB_KERNEL_ID | 0x40);
    return out + QByteArray("KERN2");
}

// connect_bootloader()'s init-connection response, lines 250-263:
// received[4] == 0x41, received[5] == 0x00 required for the "success" log
// branch -- but note NEITHER branch of this check ever returns early; a
// mismatched/short response here only logs an error and execution continues
// regardless. Minimal 6-byte length used throughout since only indices 4/5
// are ever read.
QByteArray initConnResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x41);
    out[5] = char(0x00);
    return out;
}

// connect_bootloader()'s ECU-ID response, lines 285-308: received[4] == 0xEA;
// on match, `response.remove(0,8); response.remove(5, response.length()-5);`
// extracts a fixed 5-byte "ecu id" -- needs >= 13 bytes to avoid a negative
// remove() length (same shape as the K-Line sibling's sidBfSsmInitResponse).
// Like initConnResponse(), a mismatch only logs and continues -- no early
// return.
QByteArray ecuIdResponse()
{
    QByteArray out(13, char(0));
    out[4] = char(0xEA);
    return out;
}

// connect_bootloader()'s VIN response, lines 326-343: received[4]==0x49,
// received[5]==0x02; response.remove(0,7) afterwards is safe for any length
// (Qt's QByteArray::remove clamps rather than going OOB). No early return on
// mismatch.
QByteArray vinResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x49);
    out[5] = char(0x02);
    return out;
}

// connect_bootloader()'s CAL-ID response, lines 357-374: received[4]==0x49,
// received[5]==0x04. No early return on mismatch.
QByteArray calIdResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x49);
    out[5] = char(0x04);
    return out;
}

// connect_bootloader()'s CVN response, lines 392-410: received[4]==0x49,
// received[5]==0x06. No early return on mismatch.
QByteArray cvnResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x49);
    out[5] = char(0x06);
    return out;
}

// connect_bootloader()'s session-mode-0x03 response, lines 432-446:
// received[4]==0x50, received[5]==0x03 sets req_10_03_connected=true. No
// early return on mismatch (the flag simply stays false).
QByteArray session03Response()
{
    QByteArray out(6, char(0));
    out[4] = char(0x50);
    out[5] = char(0x03);
    return out;
}

// connect_bootloader()'s session-mode-0x43 response, lines 459-473:
// received[4]==0x50, received[5]==0x43 sets req_10_43_connected=true. No
// early return on mismatch.
QByteArray session43Response()
{
    QByteArray out(6, char(0));
    out[4] = char(0x50);
    out[5] = char(0x43);
    return out;
}

// connect_bootloader()'s seed-request response, lines 486-521: received[4]==
// 0x67, received[5]==0x01, seed at received[6..9]. UNLIKE the checks above,
// a mismatch/short response here DOES `return STATUS_ERROR` (lines 504,511).
QByteArray seedResponse(const QByteArray& seed)
{
    QByteArray out(10, char(0));
    out[4] = char(0x67);
    out[5] = char(0x01);
    out[6] = seed.at(0);
    out[7] = seed.at(1);
    out[8] = seed.at(2);
    out[9] = seed.at(3);
    return out;
}

// connect_bootloader()'s seed-key-ack response, lines 556-582: received[4]==
// 0x67, received[5]==0x02. Mismatch/short response returns STATUS_ERROR
// (lines 574,581).
QByteArray seedKeyAckResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x67);
    out[5] = char(0x02);
    return out;
}

// connect_bootloader()'s final session-set response, lines 605-631:
// received[4]==0x50 && (received[5]==0x02 || received[5]==0x42). Mismatch/
// short response returns STATUS_ERROR (lines 623,630).
QByteArray sessionSetResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x50);
    out[5] = char(0x02);
    return out;
}

// upload_kernel()'s SID34 ack, lines 784-801: received[4]==0x74,
// received[5]==0x20. Mismatch/short returns STATUS_ERROR.
QByteArray sid34DownloadAckResponse()
{
    QByteArray out(6, char(0));
    out[4] = char(0x74);
    out[5] = char(0x20);
    return out;
}

// upload_kernel()'s per-0xB6-block response, line 853: read but NEVER
// inspected by any `if` -- content and even presence are irrelevant to
// control flow, this exists purely to keep the QQueue's ordering aligned
// with the real number of read_serial_data() calls the block loop makes.
QByteArray b6BlockUnusedResponse()
{
    return QByteArray();
}

// upload_kernel()'s "kernel started" ack, lines 872-889: received[4]==0x77.
// Mismatch/short returns STATUS_ERROR.
QByteArray sid37StartAckResponse()
{
    QByteArray out(5, char(0));
    out[4] = char(0x77);
    return out;
}

// upload_kernel()'s start-routine ack, lines 908-926: received[4]==0x71.
// Mismatch/short returns STATUS_ERROR.
QByteArray sid31RoutineAckResponse()
{
    QByteArray out(5, char(0));
    out[4] = char(0x71);
    return out;
}

// read_mem()'s "header ack" response, lines 1039-1052: received[4]==0xBE,
// received[5]==0xEF, received[8]==(SUB_KERNEL_READ_AREA|0x40)==0x43 (0x03 in
// kernelcomms.h line 25, |0x40). On match, `received.remove(0,9);
// mapdata.append(received);` -- exactly 9 bytes here means nothing survives
// that append (kept deliberately empty so the test's expected FullRomData is
// contributed entirely by eepromPagedataResponse264Bytes() below, not split
// across two fixtures).
QByteArray eepromHeaderAckResponse()
{
    QByteArray out(9, char(0));
    out[4] = char(0xBE);
    out[5] = char(0xEF);
    out[8] = char(SUB_KERNEL_READ_AREA | 0x40);
    return out;
}

// read_mem()'s pagedata-accumulation response, lines 1054-1078: reads are
// appended to `pagedata` until its length >= pagesize (0x100 == 256 for
// SH7055's single-page case) or 100 short-timeout reads have elapsed; then
// `if (pagedata.length() > 7) pagedata.remove(0, 8);` strips a flat 8-byte
// leading header (a plain strip, NOT the K-Line sibling's per-32-byte-block
// prefix/suffix framing -- read_mem()'s wire format here is much simpler).
// Queuing exactly 264 = 8 + 256 bytes in one response satisfies the length
// check on the very first dequeue, so the accumulation loop runs exactly
// once. The trailing 256 bytes are a 0x00..0xFF ramp, matching the decoded
// content this test expects in ecuCalDef.FullRomData.
QByteArray eepromPagedataResponse264Bytes()
{
    QByteArray out(8, char(0xEE)); // 8 leading bytes, stripped, unchecked
    for (int i = 0; i < 256; ++i)
    {
        out.append(char(i & 0xFF));
    }
    return out; // 264 bytes
}

// read_mem() never slices `mapdata` by cplen/skip_start -- it just appends
// whatever survived each response's own framing strip, whole. For our single-
// iteration SH7055 fixture (eepromHeaderAckResponse() contributes nothing,
// eepromPagedataResponse264Bytes() contributes the ramp after its 8-byte
// strip), ecuCalDef->FullRomData ends up exactly this 256-byte ramp.
QByteArray expectedDecodedEeprom256Bytes()
{
    QByteArray out;
    for (int i = 0; i < 256; ++i)
    {
        out.append(char(i & 0xFF));
    }
    return out;
}

// read_ram_location()'s "no valid response" fixture: `if (received.length() >
// 8) {...} else { emit LOG_E(...); return STATUS_ERROR; }` (lines 671-685).
// Deliberately used for BOTH read_ram_location() calls in the
// ecutekRaceRomAlt fixture below to force this early-return branch --
// see that fixture's comment for why the alternative (a "positive ACK"
// response) cannot be safely characterized.
QByteArray ramReadShortResponse()
{
    return QByteArray();
}

QString writeKernelFixture(QTemporaryFile& file, const QByteArray& bytes)
{
    if (!file.open())
    {
        return {};
    }
    file.write(bytes);
    const QString path = file.fileName();
    file.close();
    return path;
}

// A 16-byte (already 4-byte-aligned) kernel fixture -- same size/rationale as
// the K-Line sibling's, though this class's upload_kernel() pads pl_encr to
// exactly data_len BEFORE calling encrypt_payload() (lines 744-747), so
// (unlike the K-Line sibling) there's no latent OOB-read risk here for
// non-4-aligned files; 16 bytes is used purely for parity/readability.
QByteArray kernelFixtureBytes()
{
    QByteArray out;
    for (int i = 0; i < 16; ++i)
    {
        out.append(char(i));
    }
    return out;
}

constexpr quint32 kKernelStartAddr = 0xFFFF6004;

// Enqueues the 11 responses a full (kernel-not-yet-running) bootloader init
// round consumes, using the stock (non-variant) seed-key algorithm.
void enqueueConnectBootloaderFullInit(QueuedFakeBackend *fake, const QByteArray& seed)
{
    fake->responses.enqueue(kernelNotAliveResponse());
    fake->responses.enqueue(initConnResponse());
    fake->responses.enqueue(ecuIdResponse());
    fake->responses.enqueue(vinResponse());
    fake->responses.enqueue(calIdResponse());
    fake->responses.enqueue(cvnResponse());
    fake->responses.enqueue(session03Response());
    fake->responses.enqueue(session43Response());
    fake->responses.enqueue(seedResponse(seed));
    fake->responses.enqueue(seedKeyAckResponse());
    fake->responses.enqueue(sessionSetResponse());
}

// Enqueues the responses one upload_kernel() round consumes for a given
// kernel fixture: SID34 ack, one (unchecked) ack per 0xB6 block, 0x37 ack,
// 0x31 ack, then the post-upload kernel-alive poll.
void enqueueUploadKernel(QueuedFakeBackend *fake, const QByteArray& kernelBytes)
{
    const KernelUploadPlan plan = computeKernelUploadPlan(kernelBytes);
    fake->responses.enqueue(sid34DownloadAckResponse());
    for (int blockno = 0; blockno <= plan.maxBlocks; ++blockno)
    {
        fake->responses.enqueue(b6BlockUnusedResponse());
    }
    fake->responses.enqueue(sid37StartAckResponse());
    fake->responses.enqueue(sid31RoutineAckResponse());
    fake->responses.enqueue(kernelAliveResponse());
}

// Enqueues the 2 responses one read_mem() page (SH7055's single page) consumes.
void enqueueReadMem(QueuedFakeBackend *fake)
{
    fake->responses.enqueue(eepromHeaderAckResponse());
    fake->responses.enqueue(eepromPagedataResponse264Bytes());
}

FileActions::EcuCalDefStructure makeEcuCalDef()
{
    FileActions::EcuCalDefStructure def;
    def.McuType = "SH7055";
    def.KernelStartAddr = "FFFF6004";
    return def;
}

class SaveOnFirstPrompt
{
  public:
    static FlashOperationWorker::PromptFn make()
    {
        return [](QWidget *, const QString&, const QString&, int, int) -> int
        { return QMessageBox::Save; };
    }
};

class DiscardThenIgnitionOk
{
  public:
    static FlashOperationWorker::PromptFn make(int *callCount)
    {
        return [callCount](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++*callCount;
            // Odd calls are the "Save/Ignore downloaded content" prompt,
            // even calls are the "cycle ignition" prompt.
            return (*callCount % 2 == 1) ? QMessageBox::Ignore : QMessageBox::Ok;
        };
    }
};

} // namespace

class TestEepromCanCharacterization : public QObject
{
    Q_OBJECT

  private slots:

    void kernelAlreadyRunning_skipsBootloaderAndReadsMode2()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false); // instantiates the backend

        fake->responses.enqueue(kernelAliveResponse());
        enqueueReadMem(fake);

        auto ecuCalDef = makeEcuCalDef();
        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog,
                                                  nullptr, SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(5000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = {
            "write_echo_check:begin:" + hex(requestKernelIdRequest()),
            "write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(2)),
        };
        QCOMPARE(writes, expected);
        QCOMPARE(ecuCalDef.FullRomData, expectedDecodedEeprom256Bytes());
    }

    void fullBootloaderStockSecurity_mode2_saveEndsTheLoop()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueConnectBootloaderFullInit(fake, seed);

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        enqueueUploadKernel(fake, kernelFixtureBytes());
        enqueueReadMem(fake);

        int promptCalls = 0;
        auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++promptCalls;
            return QMessageBox::Save;
        };

        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(10000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QStringList expected = expectedConnectBootloaderFullInitWrites(generateSeedKeyStock(seed));
        expected << expectedUploadKernelWrites(kernelFixtureBytes(), kKernelStartAddr);
        expected << "write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(2));
        QCOMPARE(writes, expected);

        QCOMPARE(promptCalls, 1); // no ignition-cycle prompt -- EEPROM_MODE never advanced past 2
        QCOMPARE(ecuCalDef.FullRomData, expectedDecodedEeprom256Bytes());
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    }

    void mode2Discard_thenMode3Discard_thenMode4AllDiscarded_endsUnsuccessful()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        // Round 1 (EEPROM_MODE == 2): kernel not yet running -- full init +
        // kernel upload, then the mode-2 EEPROM read.
        enqueueConnectBootloaderFullInit(fake, seed);
        enqueueUploadKernel(fake, kernelFixtureBytes());
        enqueueReadMem(fake);
        // Round 2 (EEPROM_MODE == 3): kernel_alive is now true, so
        // connect_bootloader()'s request_kernel_id() alone succeeds and
        // upload_kernel() is skipped entirely -- straight to the read.
        fake->responses.enqueue(kernelAliveResponse());
        enqueueReadMem(fake);
        // Round 3 (EEPROM_MODE == 4): same shape as round 2.
        fake->responses.enqueue(kernelAliveResponse());
        enqueueReadMem(fake);

        int promptCalls = 0;
        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr,
                                                  DiscardThenIgnitionOk::make(&promptCalls));
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(15000));
        QVERIFY(op.wait(2000));

        const QString prefix = "write_echo_check:begin:" + sidReadEepromHexPrefix();
        const QStringList dumpWrites = fake->takeCallLog().filter(prefix);
        const QStringList expectedDumps = {
            "write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(2)),
            "write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(3)),
            "write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(4)),
        };
        QCOMPARE(dumpWrites, expectedDumps); // literal proof EEPROM_MODE progresses 2 -> 3 -> 4, never repeats/skips

        QCOMPARE(promptCalls, 6);                          // 3 rounds * (discard prompt + ignition-cycle prompt)
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false); // no round saved -- loop exhausts naturally after mode 4
    }

    void ignitionCyclePromptCancel_endsTheLoopWithoutAFourthRound()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        enqueueConnectBootloaderFullInit(fake, seed);
        enqueueUploadKernel(fake, kernelFixtureBytes());
        enqueueReadMem(fake);

        int promptCalls = 0;
        auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++promptCalls;
            // First prompt (Save/Ignore downloaded content) -> discard.
            // Second prompt (cycle ignition) -> Cancel.
            return (promptCalls == 1) ? QMessageBox::Ignore : QMessageBox::Cancel;
        };

        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(10000));
        QVERIFY(op.wait(2000));

        const QString prefix = "write_echo_check:begin:" + sidReadEepromHexPrefix();
        const QStringList dumpWrites = fake->takeCallLog().filter(prefix);
        QCOMPARE(dumpWrites, QStringList{"write_echo_check:begin:" + hex(sidReadEepromRequestForSh7055(2))});

        QCOMPARE(promptCalls, 2); // discard, then Cancel on the ignition-cycle prompt -- no round 2
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
    }

    void testWriteCommand_uploadsKernelButNeverWritesEeprom()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueConnectBootloaderFullInit(fake, seed);
        enqueueUploadKernel(fake, kernelFixtureBytes());
        // No further response queued: cmd_type == "test_write" never reaches
        // read_mem() (eeprom_ecu_subaru_denso_sh705x_can_operation.cpp:89-94
        // -- the write_mem() call is commented out and `result` is left
        // untouched from upload_kernel()'s STATUS_SUCCESS), so no
        // READ_EEPROM write should ever occur.

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "test_write", &dialog, nullptr,
                                                  SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(10000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = expectedConnectBootloaderFullInitWrites(generateSeedKeyStock(seed)) +
                                     expectedUploadKernelWrites(kernelFixtureBytes(), kKernelStartAddr);
        QCOMPARE(writes, expected); // upload_kernel()'s writes occur; no trailing READ_EEPROM write
        QVERIFY(writes.filter("write_echo_check:begin:" + sidReadEepromHexPrefix()).isEmpty());

        // Legacy gap this pins: the operation reports success despite never
        // writing anything to the ECU's EEPROM.
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    }

    void writeCommand_uploadsKernelButNeverWritesEeprom()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        const QByteArray seed = QByteArray::fromHex("11223344");
        enqueueConnectBootloaderFullInit(fake, seed);
        enqueueUploadKernel(fake, kernelFixtureBytes());

        QTemporaryFile kernelFile;
        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.Kernel = writeKernelFixture(kernelFile, kernelFixtureBytes());
        QVERIFY(!ecuCalDef.Kernel.isEmpty());

        QWidget dialog;
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "write", &dialog, nullptr,
                                                  SaveOnFirstPrompt::make());
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(10000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        const QStringList expected = expectedConnectBootloaderFullInitWrites(generateSeedKeyStock(seed)) +
                                     expectedUploadKernelWrites(kernelFixtureBytes(), kKernelStartAddr);
        QCOMPARE(writes, expected);
        QVERIFY(writes.filter("write_echo_check:begin:" + sidReadEepromHexPrefix()).isEmpty());
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true); // same legacy gap as "test_write"
    }

    void protocolsCfgDeclaresEepromWriteAndTestWriteUnsupported()
    {
        const QByteArray envPath = qgetenv("PROTOCOLS_CFG_PATH");
        QVERIFY2(!envPath.isEmpty(),
                 "PROTOCOLS_CFG_PATH not set -- see bazel/mut_dma_test_suites.bzl's data/env "
                 "wiring for this suite");

        QFile cfg(QString::fromLocal8Bit(envPath));
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(cfg.readAll());

        for (const QString& name : {
                 "sub_ecu_eeprom_denso_sh7055_kline", "sub_ecu_eeprom_denso_sh7058_kline",
                 "sub_ecu_eeprom_denso_sh7055_densocan", "sub_ecu_eeprom_denso_sh7058_densocan",
                 "sub_ecu_eeprom_denso_sh7058_can", "sub_ecu_eeprom_denso_sh7058_can_diesel"})
        {
            const int nameIndex = contents.indexOf(QString("name=\"%1\"").arg(name));
            QVERIFY2(nameIndex >= 0, qPrintable(name));
            const int entryEnd = contents.indexOf("</protocol>", nameIndex);
            const QString entry = contents.mid(nameIndex, entryEnd - nameIndex);
            QVERIFY2(entry.contains("<test_write>no</test_write>"), qPrintable(name));
            QVERIFY2(entry.contains("<write>no</write>"), qPrintable(name));
        }
    }

    // CAN-only fixture #1: pins that connect_bootloader()'s security-variant
    // branch (lines 522-541) really does route to a distinct
    // generate_*_seed_key() function per flash_method suffix, by driving the
    // operation far enough to capture each variant's "0x27,0x02,<key>" frame
    // from an IDENTICAL seed and proving all four differ.
    //
    // Each run is deliberately stopped right after the seed-key-send write:
    // responses are queued only through the seed-request step, so the
    // seed-key-send's own response read gets an empty QByteArray (queue
    // exhausted) and connect_bootloader() returns STATUS_ERROR immediately
    // after -- keeping this test fast without needing a full kernel upload +
    // EEPROM read per variant. (The operation's outer for-loop still runs
    // rounds 2 and 3, each failing quickly the same way, since result never
    // reaches STATUS_SUCCESS; that's expected and asserted below.)
    void allFourSecurityVariants_produceDistinctSeedKeyFrames()
    {
        const QByteArray seed = QByteArray::fromHex("11223344");
        const QStringList suffixes = {"", "_ecutek", "_cobb", "_ecutek_racerom"};
        QStringList seedKeyFrames;

        for (const QString& suffix : suffixes)
        {
            QueuedFakeBackend *fake = nullptr;
            SerialPortActions serial("", "", nullptr, nullptr,
                                     [&fake]() -> SerialBackend *
                                     { fake = new QueuedFakeBackend(); return fake; });
            serial.set_add_ssm_header(false);

            fake->responses.enqueue(kernelNotAliveResponse());
            fake->responses.enqueue(initConnResponse());
            fake->responses.enqueue(ecuIdResponse());
            fake->responses.enqueue(vinResponse());
            fake->responses.enqueue(calIdResponse());
            fake->responses.enqueue(cvnResponse());
            fake->responses.enqueue(session03Response());
            fake->responses.enqueue(session43Response());
            fake->responses.enqueue(seedResponse(seed));

            auto ecuCalDef = makeEcuCalDef();
            ecuCalDef.FlashMethod = suffix;

            int promptCalls = 0;
            QWidget dialog;
            auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
            {
                ++promptCalls;
                return QMessageBox::Save;
            };
            EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
            QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

            op.start();
            QVERIFY(finishedSpy.wait(15000));
            QVERIFY(op.wait(2000));

            QCOMPARE(promptCalls, 0);                          // result never reached STATUS_SUCCESS
            QCOMPARE(finishedSpy.at(0).at(0).toBool(), false); // confirms the "stopped short" setup above

            const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:0000");
            QVERIFY2(writes.size() >= 10, qPrintable(suffix));
            // Write #10 (0-indexed #9) is the seed-key-send frame: kernel_id,
            // init, ecuid, vin, calid, cvn, session03, session43, seedreq,
            // then seedkey-send.
            seedKeyFrames << writes.at(9);
        }

        for (int i = 0; i < seedKeyFrames.size(); ++i)
        {
            for (int j = i + 1; j < seedKeyFrames.size(); ++j)
            {
                QVERIFY2(seedKeyFrames.at(i) != seedKeyFrames.at(j),
                         qPrintable(QString("variants %1 and %2 produced the same seed-key frame")
                                        .arg(suffixes.at(i), suffixes.at(j))));
            }
        }

        // Cross-check against the real algorithms (transcribed above) so a
        // failure here points at a specific mismatched table, not just "some
        // pair collided".
        QCOMPARE(seedKeyFrames.at(0), "write_echo_check:begin:" + hex(seedKeySendRequest(generateSeedKeyStock(seed))));
        QCOMPARE(seedKeyFrames.at(1),
                 "write_echo_check:begin:" + hex(seedKeySendRequest(generateEcutekSeedKeyPlain(seed))));
        QCOMPARE(seedKeyFrames.at(2), "write_echo_check:begin:" + hex(seedKeySendRequest(generateCobbSeedKey(seed))));
        QCOMPARE(seedKeyFrames.at(3),
                 "write_echo_check:begin:" + hex(seedKeySendRequest(generateEcutekRacecomCanSeedKey(seed))));
    }

    // CAN-only fixture #2: pins connect_bootloader()'s "_ecutek_racerom_alt"
    // branch (lines 195-234) -- serial reconfigured to K-Line-like settings
    // (is_iso15765_connection(false), baud 4800), two read_ram_location()
    // calls (0xffff1ed8, 0xffff1e80), then reconfigured back
    // (is_iso15765_connection(true)) before the normal UDS-over-CAN sequence
    // resumes.
    //
    // read_ram_location() (lines 637-694) has a documented pre-existing bug:
    // it computes its result from `response` -- a QByteArray declared at
    // line 641 and NEVER populated anywhere in the function -- instead of
    // `received`, the QByteArray actually filled by read_serial_data(). The
    // buggy line only runs if `received.length() > 8` (i.e. a well-formed
    // ACK); a short/empty response instead hits the earlier `else` branch
    // (lines 680-685) and returns STATUS_ERROR (0x01) WITHOUT ever touching
    // `response`. We deliberately supply a short response to both
    // read_ram_location() calls, exercising that safe, real, reachable
    // branch -- NOT the buggy one. This is not "inventing a working
    // response": it's a genuine production behavior (any malformed RAM-read
    // ACK takes this path) that happens to also sidestep undefined behavior.
    //
    // We do NOT attempt to characterize the "well-formed ACK" branch: since
    // `response` is always empty there, `response.at(5)` is an out-of-bounds
    // QByteArray access -- genuinely undefined behavior (a Q_ASSERT abort in
    // a debug Qt build, a silent OOB heap read in a release build). Its
    // outcome depends on build configuration, not on the ECU protocol, so it
    // cannot be pinned as a stable golden fixture; per this task's brief,
    // that branch is left uncharacterized rather than fabricated.
    //
    // A second bug this fixture pins as a side effect: read_ram_location()'s
    // early-return value (STATUS_ERROR == 1) is indistinguishable from a
    // genuine "RAM value of 1" to its caller -- connect_bootloader() assigns
    // it straight into seed_alter/xor_byte_1/xor_byte_2 with no error check
    // (lines 212-218), so a failed RAM read silently poisons the seed-key
    // derivation rather than aborting the connect attempt.
    void ecutekRaceRomAlt_readsRamLocationsBeforeReconfiguringToCan()
    {
        const QByteArray seed = QByteArray::fromHex("11223344");

        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        // Round 1 (EEPROM_MODE == 2): kernel not alive -> _ecutek_racerom_alt
        // branch -> two short-circuited RAM reads -> normal init through the
        // seed request. No response queued for the seed-key-send step (mirrors
        // allFourSecurityVariants_...'s "stop right after capturing the frame
        // we care about" shape) -- connect_bootloader() returns STATUS_ERROR
        // right after, ending round 1 without a kernel upload or EEPROM read.
        fake->responses.enqueue(kernelNotAliveResponse());
        fake->responses.enqueue(ramReadShortResponse()); // read_ram_location(0xffff1ed8)
        fake->responses.enqueue(ramReadShortResponse()); // read_ram_location(0xffff1e80)
        fake->responses.enqueue(initConnResponse());
        fake->responses.enqueue(ecuIdResponse());
        fake->responses.enqueue(vinResponse());
        fake->responses.enqueue(calIdResponse());
        fake->responses.enqueue(cvnResponse());
        fake->responses.enqueue(session03Response());
        fake->responses.enqueue(session43Response());
        fake->responses.enqueue(seedResponse(seed));
        // Rounds 2 and 3 repeat the same shape (flash_method is set once,
        // outside execute()'s for-loop, so every round re-enters the
        // _ecutek_racerom_alt branch); queue the same 11 responses twice more
        // so all 3 rounds run to completion deterministically rather than
        // draining the queue and getting all-empty responses partway through
        // an unrelated step.
        for (int round = 0; round < 2; ++round)
        {
            fake->responses.enqueue(kernelNotAliveResponse());
            fake->responses.enqueue(ramReadShortResponse());
            fake->responses.enqueue(ramReadShortResponse());
            fake->responses.enqueue(initConnResponse());
            fake->responses.enqueue(ecuIdResponse());
            fake->responses.enqueue(vinResponse());
            fake->responses.enqueue(calIdResponse());
            fake->responses.enqueue(cvnResponse());
            fake->responses.enqueue(session03Response());
            fake->responses.enqueue(session43Response());
            fake->responses.enqueue(seedResponse(seed));
        }

        auto ecuCalDef = makeEcuCalDef();
        ecuCalDef.FlashMethod = "_ecutek_racerom_alt";

        int promptCalls = 0;
        QWidget dialog;
        auto prompt = [&promptCalls](QWidget *, const QString&, const QString&, int, int) -> int
        {
            ++promptCalls;
            return QMessageBox::Save;
        };
        EepromEcuSubaruDensoSH705xCanOperation op(&serial, &ecuCalDef, "read", &dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(20000));
        QVERIFY(op.wait(2000));

        QCOMPARE(promptCalls, 0); // result never reached STATUS_SUCCESS in any of the 3 rounds
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QVERIFY(writes.size() >= 12);

        // Both read_ram_location() requests were SSM/addHeader-framed (no
        // CAN-ID prefix) -- direct proof is_iso15765_connection() was false
        // when they were built, i.e. the reconfigure-to-K-Line-like-settings
        // really happened before them.
        QCOMPARE(writes.at(0), "write_echo_check:begin:" + hex(requestKernelIdRequest()));
        QCOMPARE(writes.at(1), "write_echo_check:begin:" + hex(ramReadRequest(0xffff1ed8)));
        QCOMPARE(writes.at(2), "write_echo_check:begin:" + hex(ramReadRequest(0xffff1e80)));

        // The main sequence resumes with the ordinary CAN-prefixed frames
        // right after -- i.e. reconfiguring back to CAN didn't disrupt
        // anything downstream.
        QCOMPARE(writes.at(3), "write_echo_check:begin:" + hex(initConnectionRequest()));
        QCOMPARE(writes.at(4), "write_echo_check:begin:" + hex(ecuIdRequest()));
        QCOMPARE(writes.at(5), "write_echo_check:begin:" + hex(vinRequest()));
        QCOMPARE(writes.at(6), "write_echo_check:begin:" + hex(calIdRequest()));
        QCOMPARE(writes.at(7), "write_echo_check:begin:" + hex(cvnRequest()));
        QCOMPARE(writes.at(8), "write_echo_check:begin:" + hex(sessionMode03Request()));
        QCOMPARE(writes.at(9), "write_echo_check:begin:" + hex(sessionMode43Request()));
        QCOMPARE(writes.at(10), "write_echo_check:begin:" + hex(seedRequestFrame()));

        // Both short-circuited RAM reads return STATUS_ERROR == 1 (see the
        // comment above this test): seed_alter = 1, xor_byte_1 = (1>>8)&0xff
        // = 0, xor_byte_2 = 1&0xff = 1. The resulting seed-key-send frame
        // must match generate_ecutek_seed_key()'s "_ecutek_racerom_alt"
        // branch fed exactly those constants -- proving that branch (not the
        // plain EcuTek path) was reached.
        const QByteArray expectedKey = generateEcutekRaceromAltSeedKey(seed, /*seedAlter=*/1, /*xorByte1=*/0,
                                                                       /*xorByte2=*/1);
        QCOMPARE(writes.at(11), "write_echo_check:begin:" + hex(seedKeySendRequest(expectedKey)));

        // Sanity check that the "alt" branch really does diverge from the
        // plain EcuTek key for this seed (otherwise the assertion above
        // would pass vacuously even if the alt branch were never reached).
        QVERIFY(generateEcutekRaceromAltSeedKey(seed, 1, 0, 1) != generateEcutekSeedKeyPlain(seed));
    }
};

int run_test_eeprom_ecu_subaru_denso_sh705x_can_operation_characterization(int argc, char **argv)
{
    // EepromEcuSubaruDensoSH705xCanOperation's constructor takes a QWidget*
    // dialog, and the tests below construct a real QWidget -- that requires a
    // QApplication rather than a plain QCoreApplication (same fix as
    // test_flash_ecu_mitsu_m32r_can_operation.cpp's run function).
    QApplication app(argc, argv);
    TestEepromCanCharacterization t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_eeprom_ecu_subaru_denso_sh705x_can_operation_characterization.moc"
