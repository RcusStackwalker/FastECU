#pragma once
#include <QString>
#include "src/backend/definitions/file_actions.h"

// Converts one already-assembled raw decimal value string into its display-ready
// form using the RomRaider-format from-byte expression and decimal format stored
// in logValues->log_value_units.at(j) (comma-separated: name,unit,expr,format).
// Shared by SsmLoggingProtocol, MutDmaLoggingProtocol, and CdbgLoggingProtocol,
// which each assemble rawValueString differently (SSM concatenates raw bytes as
// decimal digits per byte; MUT/DMA and Cdbg convert a single decoded quint32 to
// one decimal string) but apply the identical conversion from that point on.
QString convertRomRaiderValue(FileActions *fileActions, FileActions::LogValuesStructure *logValues,
                              int j, const QString& rawValueString);
