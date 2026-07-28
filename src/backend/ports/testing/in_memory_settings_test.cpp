#include "src/backend/ports/testing/in_memory_settings.h"
#include <gtest/gtest.h>

TEST(Settings, GetMissingReturnsNullopt)
{
    fastecu::InMemorySettings s;
    EXPECT_FALSE(s.get("k").has_value());
    s.set("k", "v");
    ASSERT_TRUE(s.get("k").has_value());
    EXPECT_EQ(*s.get("k"), "v");
}
