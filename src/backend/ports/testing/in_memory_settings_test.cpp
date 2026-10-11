#include "src/backend/ports/testing/in_memory_settings.h"
#include <gtest/gtest.h>

TEST(Settings, GetMissingReturnsNullopt)
{
    fastecu::InMemorySettings s;
    EXPECT_FALSE(s.Get("k").has_value());
    s.Set("k", "v");
    EXPECT_EQ(s.Get("k"), "v");
}
