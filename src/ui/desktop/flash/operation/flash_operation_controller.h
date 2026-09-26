#pragma once

#include <QObject>
#include <QString>

#include <optional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_types.h"

class QWidget;
class SerialPortActions;

namespace fastecu::flash
{

struct FlashOperationInput
{
    FlashOperation operation;
    std::string protocol;
    std::string mcu;
    std::string kernel_path; // for the Denso TCU "Dump" log line
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
};

enum class FlashOperationStatus
{
    Completed,            // a workflow ran; read_bytes/rom_id as the dialog returned them
    ServiceActionHandled, // a Denso TCU service action consumed the request
    Unsupported,          // no workflow for this protocol; warning shown
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
    FlashOperationController(SerialPortActions& serial, QWidget *dialog_parent);

    FlashOperationOutcome run(const FlashOperationInput& input);

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void external_logger(QString message);
    void external_logger(int value);

  private:
    SerialPortActions& serial_;
    QWidget *dialog_parent_;
};

} // namespace fastecu::flash
