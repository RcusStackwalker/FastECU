#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_transport_source.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::flash
{

// Read workflows do not consume a ROM image. Both write modes do, and must
// receive the same current calibration bytes from desktop request construction.
std::optional<bytes::Bytes> PortableImageForOperation(FlashOperation operation, bytes::ByteView rom);

struct FlashWorkflowRequest
{
    FlashOperation operation;
    config::ProtocolSpec protocol;
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
};

enum class FlashPromptKind
{
    kBegin,
    kColtEraseTrigger,
    kColtTopRegionBootstrap,
    kInspectRead,
    kCycleIgnition,
    kConfirmSh7058Read,
    kConfirmBdmKernelBootstrap,
    // Before the attempt: the operator applies external VPP.
    kApplyProgrammingVoltage,
    // After a write attempt, OK-only. Due after every write that
    // did not succeed, whatever the adapter, and after a successful write
    // when external VPP was needed. Argument "outcome" is "succeeded",
    // "failed" or "cancelled"; "external_vpp" is "yes" when the operator
    // applied VPP (the notice asks for its removal) or "no" otherwise. Every
    // outcome other than "succeeded" carries the don't-power-off advice
    // unless "power_off_advice" is "no" (bootmode: the boot ROM is
    // always re-enterable, so a power cycle is the recovery path).
    kRemoveProgrammingVoltage,
    // Before a bootmode write: the operator connects VPP and MOD1.
    kApplyBootModeVoltages,
    // Between the bootmode kernel upload and programming, OK/Cancel.
    kRemoveMod1,
};

enum class FlashPromptResponse
{
    kAccept,
    kDecline,
    kSave,
    kDiscard,
};

enum class FlashWorkflowOutcome
{
    kSucceeded,
    kCancelled,
    kDiscarded,
    kFailed,
};

// What a workflow needs from its platform, owned by the workflow: `files`
// reads kernel files by handle, and `transports` makes the transport each
// attempt is bound to. Both must be non-null.
struct FlashWorkflowPorts
{
    std::unique_ptr<IFileRepository> files;
    std::unique_ptr<IFlashTransportSource> transports;
};

struct FlashPromptStep
{
    FlashPromptKind kind;
    std::vector<std::pair<std::string, std::string>> arguments;
};

struct FlashAttempt
{
    std::unique_ptr<BoundFlashAttempt> attempt;
};

struct FlashAttemptResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::kInternal;
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
    virtual FlashWorkflowStep Next() = 0;
    virtual void Submit(FlashPromptResponse response) = 0;
    virtual void Submit(FlashAttemptResult result) = 0;
};

class FlashWorkflowFactory
{
  public:
    // A null result means no flash route names this protocol id. A routed
    // protocol returns a workflow even when preflight will fail.
    static std::unique_ptr<FlashWorkflow> TryCreate(FlashWorkflowRequest request, FlashWorkflowPorts ports);
    // Every id of every flash route, duplicates kept, in registry order.
    static std::vector<std::string_view> RoutedProtocolIds();
};

} // namespace fastecu::flash
