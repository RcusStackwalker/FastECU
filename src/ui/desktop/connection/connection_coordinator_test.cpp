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

// Capability parsing can open a notice whose nested event loop starts another
// connection. The outer attempt then reports "not connected" and must not
// unlock controls the nested attempt locked.
TEST_F(ConnectionCoordinatorTest, NestedBeginInsideIdentifiedKeepsControlsLockedAndFailsTheOuterAttempt)
{
    presentation.on_identified = [this](const IdentifyOutcome&)
    { coordinator.begin(SsmIdentifyRequest{}, continuation("2")); };
    coordinator.begin(SsmIdentifyRequest{}, continuation("1"));
    launcher.complete(1, success("OUTER"));

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1", "stop_and_join", "identified OUTER", "controls_locked=1",
                              "start 2", "done1=0"}));
    EXPECT_TRUE(coordinator.identifying());

    // The nested attempt owns its own continuation. Drop the re-entry hook so
    // its own identification does not start a third attempt.
    presentation.on_identified = nullptr;
    events.clear();
    launcher.complete(2, success("INNER"));

    EXPECT_EQ(events, (Events{"stop_and_join", "identified INNER", "done2=1", "controls_locked=0"}));
}

TEST_F(ConnectionCoordinatorTest, NestedCancelInsideIdentifiedFailsTheOuterAttempt)
{
    presentation.on_identified = [this](const IdentifyOutcome&) { coordinator.cancel(); };
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, success());

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1", "stop_and_join", "identified 3152584006", "done=0",
                              "controls_locked=0"}));
}

// disconnect_from_ecu() runs from identification_failed and cancels: nothing is
// running by then, so it only advances the generation and the result stays
// "connected". Passes before the change; pins the `!success` clause.
TEST_F(ConnectionCoordinatorTest, DisconnectFromTheFailureCallbackDoesNotFlipTheResult)
{
    presentation.on_failed = [this](const IdentifyOutcome&) { coordinator.cancel(); };
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, failure());

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 1", "stop_and_join", "failed no answer", "done=1",
                              "controls_locked=0"}));
}

TEST_F(ConnectionCoordinatorTest, DuplicateCompletionIsIgnored)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    launcher.complete(1, success());
    events.clear();
    launcher.complete(1, success());

    EXPECT_TRUE(events.empty());
}

TEST_F(ConnectionCoordinatorTest, SequentialAttemptsOwnTheirContinuations)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation("1"));
    launcher.complete(1, success());
    coordinator.cancel();
    events.clear();

    coordinator.begin(SsmIdentifyRequest{}, continuation("2"));
    launcher.complete(3, success());

    EXPECT_EQ(events, (Events{"controls_locked=1", "start 3", "stop_and_join", "identified 3152584006", "done2=1",
                              "controls_locked=0"}));
}

TEST_F(ConnectionCoordinatorTest, ShutdownDropsTheContinuationButStillJoinsAndUnlocks)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    coordinator.shutdown();

    EXPECT_EQ(events,
              (Events{"controls_locked=1", "start 1", "stop_and_join", "controls_locked=0", "port_selector=1"}));
}

TEST_F(ConnectionCoordinatorTest, CompletionAfterShutdownIsDropped)
{
    coordinator.begin(SsmIdentifyRequest{}, continuation());
    coordinator.shutdown();
    events.clear();
    launcher.complete(1, success());

    EXPECT_TRUE(events.empty());
}

} // namespace
