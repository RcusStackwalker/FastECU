#include "src/ui/desktop/connection/connection_coordinator.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/ui/desktop/connection/testing/fake_connection_ports.h"

namespace
{

using fastecu::diagnostics::SsmIdentifyRequest;
using fastecu::ui::ConnectionCoordinator;
using fastecu::ui::FakeConnectionPresentation;
using fastecu::ui::FakeIdentifyLauncher;
using fastecu::ui::IdentifyOutcome;
using Events = std::vector<std::string>;

IdentifyOutcome success(std::string ecu_id = "3152584006")
{
    IdentifyOutcome outcome;
    outcome.success = true;
    outcome.ecu_id = std::move(ecu_id);
    return outcome;
}

IdentifyOutcome failure(std::string detail = "no answer")
{
    IdentifyOutcome outcome;
    outcome.error_detail = std::move(detail);
    return outcome;
}

class ConnectionCoordinatorTest : public ::testing::Test
{
  protected:
    // A continuation that records `done<label>=<0|1>`.
    std::function<void(bool)> continuation(std::string label = "")
    {
        return [this, label](bool connected) { events.push_back("done" + label + "=" + (connected ? "1" : "0")); };
    }

    Events events;
    FakeIdentifyLauncher launcher{events};
    FakeConnectionPresentation presentation{events};
    ConnectionCoordinator coordinator{launcher, presentation};
};

TEST_F(ConnectionCoordinatorTest, BeginLocksControlsThenStartsTheFirstGeneration)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1"}));
    EXPECT_TRUE(coordinator.identifying());
}

TEST_F(ConnectionCoordinatorTest, SuccessReportsConnectedAndUnlocksButKeepsThePortSelectorLocked)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, success());

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1", "stop_and_join", "identified 3152584006", "done=1",
                              "controls_locked=0"}));
    EXPECT_FALSE(coordinator.identifying());
}

TEST_F(ConnectionCoordinatorTest, FailureStillReportsConnectedBecauseThePortOpened)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, failure("boom"));

    EXPECT_EQ(events,
              (Events{"controls_locked=1", "start 1", "stop_and_join", "failed boom", "done=1", "controls_locked=0"}));
}

TEST_F(ConnectionCoordinatorTest, SuccessWithAnEmptyEcuIdStillReportsConnected)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, success(""));

    EXPECT_EQ(events,
              (Events{"controls_locked=1", "start 1", "stop_and_join", "identified ", "done=1", "controls_locked=0"}));
}

TEST_F(ConnectionCoordinatorTest, CancelJoinsUnlocksEverythingAndFiresTheContinuationLast)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    coordinator.cancel();

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1", "stop_and_join", "controls_locked=0", "port_selector=1",
                              "done=0"}));
    EXPECT_FALSE(coordinator.identifying());
}

TEST_F(ConnectionCoordinatorTest, CancelTwiceFiresTheContinuationOnce)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    coordinator.cancel();
    events.clear();
    coordinator.cancel();

    EXPECT_TRUE(events.empty());
}

TEST_F(ConnectionCoordinatorTest, CompletionAfterCancelIsDropped)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    coordinator.cancel();
    events.clear();
    launcher.complete(1, success());

    EXPECT_TRUE(events.empty());
}

TEST_F(ConnectionCoordinatorTest, CancelWhileIdleStillAdvancesTheGeneration)
{
    coordinator.cancel();
    EXPECT_TRUE(events.empty());

    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, success());

    EXPECT_EQ(launcher.started_generations, (std::vector<fastecu::ui::IdentifyGeneration>{2}));
    EXPECT_EQ(events, (Events{"controls_locked=1", "start 2"}));
}

TEST(ConnectionCoordinatorLifetimeTest, DestructorDetachesFromTheLauncher)
{
    Events events;
    FakeIdentifyLauncher launcher{events};
    FakeConnectionPresentation presentation{events};
    {
        ConnectionCoordinator coordinator{launcher, presentation};
        EXPECT_TRUE(launcher.has_handler());
    }
    EXPECT_FALSE(launcher.has_handler());
}

} // namespace
