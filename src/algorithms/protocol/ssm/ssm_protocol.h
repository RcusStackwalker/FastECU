#pragma once

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <QByteArray>
#include <QtGlobal>

#include <cstdint>

namespace SsmProtocol
{

QByteArray calculateSeedKey(const QByteArray& seed, SeedKeyToGenerateIndex keytogenerateindex,
                            IndexTransformation indextransformation);
QByteArray calculatePayload(const QByteArray& buf, uint32_t len,
                            KeyToGenerateIndex keytogenerateindex,
                            IndexTransformation indextransformation);
QByteArray addHeader(const QByteArray& output, uint8_t testerId, uint8_t targetId);
bool hasValidFrame(const QByteArray& frame, uint8_t receiverId, uint8_t senderId);
bool hasPayloadPrefix(const QByteArray& frame, const QByteArray& prefix,
                      uint8_t receiverId, uint8_t senderId);

} // namespace SsmProtocol
