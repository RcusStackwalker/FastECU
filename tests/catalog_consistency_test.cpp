// The built-in catalog against the three things it must agree with and no
// single production package owns: checksum routing, the kernel bundle, and
// the flash memory models.
#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "src/backend/checksum/dispatch.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace
{

using fastecu::config::BuiltinCatalog;
using fastecu::config::ChecksumSupport;

TEST(CatalogConsistency, ChecksumFlagAgreesWithChecksumRouting)
{
    for (const auto& vehicle : BuiltinCatalog().Vehicles())
    {
        const bool routed = fastecu::checksum::HasRoute(vehicle.make, vehicle.protocol->name);
        EXPECT_EQ(vehicle.protocol->checksum == ChecksumSupport::kCorrected, routed)
            << vehicle.id << " (" << vehicle.protocol->name << ")";
    }
}

// File names exactly as Bazel bundles them, from
// $(locations //resources/shared:kernel_files): compared as strings, so a
// case-insensitive filesystem cannot hide a mismatch.
std::set<std::string> bundled_kernel_names()
{
    std::set<std::string> names;
    const char *paths = std::getenv("KERNEL_FILES");
    if (paths == nullptr)
    {
        return names;
    }
    std::istringstream stream{paths};
    for (std::string path; stream >> path;)
    {
        names.insert(path.substr(path.find_last_of('/') + 1));
    }
    return names;
}

TEST(CatalogConsistency, EveryKernelNameIsABundledFileSpelledExactly)
{
    const std::set<std::string> bundled = bundled_kernel_names();
    ASSERT_FALSE(bundled.empty());
    for (const auto& protocol : BuiltinCatalog().Protocols())
    {
        if (!protocol.kernel.empty())
        {
            EXPECT_TRUE(bundled.contains(std::string(protocol.kernel))) << protocol.name << ": " << protocol.kernel;
        }
    }
}

TEST(CatalogConsistency, EveryBundledKernelIsUsed)
{
    for (const std::string& name : bundled_kernel_names())
    {
        EXPECT_TRUE(std::ranges::any_of(BuiltinCatalog().Protocols(),
                                        [&name](const auto& protocol) { return protocol.kernel == name; }))
            << name;
    }
}

// MUT/DMA logging's M32170 is the one MCU without a flash memory model, and
// that protocol offers no flash operation.
TEST(CatalogConsistency, EveryFlashCapableProtocolNamesAKnownMcu)
{
    for (const auto& protocol : BuiltinCatalog().Protocols())
    {
        if (protocol.read || protocol.test_write || protocol.write)
        {
            EXPECT_NE(fastecu::flash::FindFlashDevice(protocol.mcu), nullptr) << protocol.name << ": " << protocol.mcu;
        }
    }
}

} // namespace
