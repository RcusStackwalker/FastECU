#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"

class SerialPortActions;

namespace fastecu::flash
{

// Read workflows do not consume a ROM image. Both write modes do, and must
// receive the same current calibration bytes from desktop request construction.
std::optional<bytes::Bytes> portableImageForOperation(FlashOperation operation, bytes::ByteView rom);

struct FlashWorkflowRequest
{
    FlashOperation operation;
    std::string protocol;
    std::string mcu;
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
    SerialPortActions *serial = nullptr;
};

enum class FlashPromptKind
{
    Begin,
    ColtEraseTrigger,
    ColtTopRegionBootstrap,
    InspectRead,
    CycleIgnition,
};

enum class FlashPromptResponse
{
    Accept,
    Decline,
    Save,
    Discard,
};

enum class FlashWorkflowOutcome
{
    Succeeded,
    Cancelled,
    Discarded,
    Failed,
};

struct FlashPromptStep
{
    FlashPromptKind kind;
    std::vector<std::pair<std::string, std::string>> arguments;
};

struct FlashAttempt
{
    std::unique_ptr<BoundFlashAttempt> attempt;
    std::unique_ptr<IClock> clock;
};

struct FlashAttemptResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    std::string error_detail;
    std::optional<bytes::Bytes> read_bytes;
    std::optional<std::string> rom_id;
};

struct FlashCompletedStep
{
    FlashWorkflowOutcome outcome;
    std::optional<bytes::Bytes> accepted_read_bytes;
    std::optional<std::string> rom_id;
};

struct FlashFailureStep
{
    Error error;
};

using FlashWorkflowStep = std::variant<FlashPromptStep, FlashAttempt, FlashCompletedStep, FlashFailureStep>;

class FlashWorkflow
{
  public:
    virtual ~FlashWorkflow() = default;
    virtual FlashWorkflowStep next() = 0;
    virtual void submit(FlashPromptResponse response) = 0;
    virtual void submit(FlashAttemptResult result) = 0;
};

class FlashWorkflowFactory
{
  public:
    // A null result means only that no portable family owns this protocol.
    // Recognized families return a workflow even when preflight will fail.
    static std::unique_ptr<FlashWorkflow> tryCreate(FlashWorkflowRequest request);
};

} // namespace fastecu::flash
