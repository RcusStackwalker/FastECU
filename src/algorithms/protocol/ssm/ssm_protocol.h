#ifndef SSM_PROTOCOL_H
#define SSM_PROTOCOL_H

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstdint>

namespace SsmProtocol
{

QByteArray calculateSeedKey(const QByteArray& seed, const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation);
QByteArray calculatePayload(const QByteArray& buf, uint32_t len,
                            const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation);
uint8_t checksum(const QByteArray& output, bool dec0x100 = false);
QByteArray addHeader(const QByteArray& output, uint8_t testerId, uint8_t targetId,
                     bool dec0x100 = false);
bool hasValidFrame(const QByteArray& frame, uint8_t receiverId, uint8_t senderId,
                   bool dec0x100 = false);
bool hasPayloadPrefix(const QByteArray& frame, const QByteArray& prefix,
                      uint8_t receiverId, uint8_t senderId,
                      bool dec0x100 = false);
QString toHex(const QByteArray& received);
uint32_t crc32(const unsigned char *buf, uint32_t len);

} // namespace SsmProtocol

#endif // SSM_PROTOCOL_H
