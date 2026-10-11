#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cstdlib>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "src/backend/config/builtin_catalog.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{

// A kernel directory holding every bundled kernel, from
// $(locations //resources/shared:kernel_files).
std::optional<config::ConfigPaths> BundledKernelPaths(const QTemporaryDir& directory)
{
    const QString kernel_directory = directory.filePath("kernels");
    const char *paths = std::getenv("KERNEL_FILES");
    if (paths == nullptr || !QDir().mkpath(kernel_directory))
    {
        return std::nullopt;
    }
    std::istringstream stream{paths};
    for (std::string path; stream >> path;)
    {
        const QString source = QString::fromStdString(path);
        if (!QFile::copy(source, kernel_directory + "/" + QFileInfo(source).fileName()))
        {
            return std::nullopt;
        }
    }
    config::ConfigPaths result;
    result.kernel_files_directory = (kernel_directory + "/").toStdString();
    return result;
}

// Erased flash of the MCU's ROM size: right for most families, and never a
// reason for an Unsupported rejection.
std::optional<bytes::Bytes> ImageFor(const config::ProtocolSpec& protocol, FlashOperation operation)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    const FlashDevice *device = FindFlashDevice(protocol.mcu);
    return bytes::Bytes(device != nullptr ? device->romsize : 0U, 0xFF);
}

// The UI offers exactly the operations the catalog marks, so every one of
// them must reach the family, and every other one must be refused as
// Unsupported before any ECU I/O. A supported read must pass preflight,
// which proves the family accepts the entry's MCU, kernel and load address.
TEST(BuiltinCatalogCapability, EveryOfferedOperationIsAcceptedAndEveryOtherIsUnsupported)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::optional<config::ConfigPaths> paths = BundledKernelPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const config::ProtocolSpec& protocol : config::BuiltinCatalog().Protocols())
    {
        if (!protocol.read && !protocol.test_write && !protocol.write)
        {
            EXPECT_TRUE(FlashWorkflowFactory::TryCreate(
                            {.operation = FlashOperation::kRead, .protocol = protocol, .paths = *paths}) == nullptr)
                << protocol.name << " offers no operation but has a route";
            continue;
        }
        for (const auto& [operation, offered] : {std::pair{FlashOperation::kRead, protocol.read},
                                                 std::pair{FlashOperation::kTestWrite, protocol.test_write},
                                                 std::pair{FlashOperation::kWrite, protocol.write}})
        {
            SCOPED_TRACE(std::format("{} operation {}", protocol.name, static_cast<int>(operation)));
            auto workflow = FlashWorkflowFactory::TryCreate({.operation = operation,
                                                             .protocol = protocol,
                                                             .image = ImageFor(protocol, operation),
                                                             .paths = *paths,
                                                             .display_filename = "capability.bin",
                                                             .serial = nullptr});
            ASSERT_TRUE(workflow != nullptr) << "no route";
            const FlashWorkflowStep step = workflow->Next();
            const auto *failure = std::get_if<FlashFailureStep>(&step);
            if (!offered)
            {
                ASSERT_TRUE(failure != nullptr) << "an unoffered operation passed preflight";
                EXPECT_EQ(failure->error.kind, ErrorKind::kUnsupported) << failure->error.detail;
            }
            else if (operation == FlashOperation::kRead)
            {
                EXPECT_TRUE(failure == nullptr) << (failure != nullptr ? failure->error.detail : std::string{});
            }
            else if (failure != nullptr)
            {
                EXPECT_NE(failure->error.kind, ErrorKind::kUnsupported) << failure->error.detail;
            }
        }
    }
}

} // namespace
} // namespace fastecu::flash

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
