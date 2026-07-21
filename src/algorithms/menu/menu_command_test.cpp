#include "src/algorithms/menu/menu_command.h"

#include <gtest/gtest.h>

TEST(MenuCommandPortable, RoundTripsKnownCommand)
{
    const std::string id = menu_command_id(MenuCommand::OpenCalibration);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(menu_command_from_id(id), MenuCommand::OpenCalibration);
}

TEST(MenuCommandPortable, UnknownIdMapsToUnknown)
{
    EXPECT_EQ(menu_command_from_id("no_such_command"), MenuCommand::Unknown);
}

TEST(MenuCommandPortable, UnknownCommandMapsToEmptyId)
{
    EXPECT_TRUE(menu_command_id(MenuCommand::Unknown).empty());
}
