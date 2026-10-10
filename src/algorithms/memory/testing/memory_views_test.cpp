#include "src/algorithms/memory/testing/memory_views.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::memory
{
namespace
{
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

    EXPECT_FALSE(image.Write(FlashAddress{0}, bytes::Bytes{2}).has_value());
}
} // namespace
} // namespace fastecu::memory
