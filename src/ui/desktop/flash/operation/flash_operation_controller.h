#pragma once

#include <QObject>
#include <QString>

#include <optional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_types.h"

class QWidget;
class SerialPortActions;

namespace fastecu::flash
{

struct FlashOperationInput
{
    FlashOperation operation;
    config::ProtocolSpec protocol;
    std::string kernel_path; // for the Denso TCU "Dump" log line
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
};

enum class FlashOperationStatus
{
    kCompleted,            // a workflow ran; read_bytes/rom_id as the dialog returned them
    kServiceActionHandled, // a Denso TCU service action consumed the request
    kUnsupported,          // no workflow for this protocol; warning shown
};

struct FlashOperationOutcome
{
    FlashOperationStatus status;
    std::optional<bytes::Bytes> read_bytes;
    std::optional<std::string> rom_id;
};

// Runs one flash operation for MainWindow and reports what happened. Owns no
// calibration state; MainWindow applies the outcome.
class FlashOperationController : public QObject
{
    Q_OBJECT

  public:
    FlashOperationController(SerialPortActions& serial, QWidget *dialogParent);

    FlashOperationOutcome run(const FlashOperationInput& input);

  signals:
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    void externalLogger(QString message);
    void externalLogger(int value);

  private:
    SerialPortActions& serial_;
    QWidget *dialog_parent_;
};

} // namespace fastecu::flash
