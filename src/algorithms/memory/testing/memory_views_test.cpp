#include "src/algorithms/memory/testing/memory_views.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <array>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::memory
{
namespace
{
using fastecu::testing::IsErrThat;
using ::testing::_;
using ::testing::ElementsAre;

TEST(MemoryViews, ViewOfPlacesTheBytesAtTheGivenAddress)
{
    const bytes::Bytes data{1, 2, 3};
    const MemoryView view = testing::ViewOf(data, FlashAddress{0x08F9C000});

    EXPECT_EQ(view.Range().Start(), FlashAddress{0x08F9C000});
    EXPECT_THAT(view.Data(), ElementsAre(1, 2, 3));
}

TEST(MemoryViews, ImageAtHonoursWritability)
{
    MemoryImage image = testing::ImageAt(FlashAddress{0}, bytes::Bytes{1}, Writability::kReadOnly);

    EXPECT_THAT(image.Write(FlashAddress{0}, bytes::Bytes{2}), IsErrThat(_));
}

TEST(MemoryViews, IdentityImageHoldsTheFileAtAddressZero)
{
    const MemoryImage image = testing::IdentityImage(bytes::Bytes{1, 2, 3});

    EXPECT_EQ(image.Map().Span().Start(), FlashAddress{0});
    EXPECT_THAT(image.File(), ElementsAre(1, 2, 3));
}

TEST(MemoryViews, PlacedImageUsesTheBlocksAndDefinitionBase)
{
    const std::array blocks{
        MemoryBlock{.range = AddressRange<FlashSpace>::Make(FlashAddress{0x1000}, ByteCount{2}).value(),
                    .backing = FileBacking{},
                    .writability = Writability::kWritable}};

    const MemoryImage image = testing::PlacedImage(blocks, bytes::Bytes{1, 2}, FlashAddress{0x1000});

    EXPECT_EQ(image.Map().DefinitionBase(), FlashAddress{0x1000});
    EXPECT_EQ(image.Map().Span().Start(), FlashAddress{0x1000});
}
} // namespace
} // namespace fastecu::memory
