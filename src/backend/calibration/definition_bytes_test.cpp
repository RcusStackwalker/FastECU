#include "src/backend/calibration/definition_bytes.h"

#include <cstdint>
#include <limits>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

TEST(DefinitionBytes, ReadsByDefinitionAddressInsideTheWindow)
{
    const bytes::Bytes data{1, 2, 3, 4};
    const DefinitionBytes window(data, 0x1000);

    EXPECT_THAT(window.At(0x1001, 2), Optional(ElementsAre(2, 3)));
    EXPECT_THAT(window.At(0x1004, 0), Optional(IsEmpty()));
}

TEST(DefinitionBytes, RefusesBytesOutsideTheWindow)
{
    const bytes::Bytes data{1, 2, 3, 4};
    const DefinitionBytes window(data, 0x1000);

    EXPECT_EQ(window.At(0x0FFF, 1), std::nullopt);
    EXPECT_EQ(window.At(0x1003, 2), std::nullopt);
    EXPECT_EQ(window.At(std::numeric_limits<std::uint64_t>::max(), 1), std::nullopt);
}

TEST(DefinitionBytes, AByteBufferIsAWindowAtAddressZero)
{
    const bytes::Bytes data{7, 8};
    const DefinitionBytes window = data;

    EXPECT_THAT(window.At(0, 2), Optional(ElementsAre(7, 8)));
}
} // namespace
} // namespace fastecu::calibration
