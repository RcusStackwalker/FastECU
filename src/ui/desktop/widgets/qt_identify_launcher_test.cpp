#include "src/ui/desktop/widgets/qt_identify_launcher.h"

#include <QString>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"
#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

namespace
{

using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::SsmIdentifyRequest;
using fastecu::ui::IdentifyGeneration;
using fastecu::ui::IdentifyOutcome;
using fastecu::ui::QtIdentifyLauncher;

const bytes::Bytes kShortEcuInit{0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C};

struct Completion
{
    IdentifyGeneration generation;
    IdentifyOutcome outcome;
};

class QtIdentifyLauncherTest : public ::testing::Test
{
  protected:
    QtIdentifyLauncherTest()
        : launcher_(
              [this]
              {
                  auto link = std::make_unique<FakeDiagnosticLink>();
                  if (answer_ecu_)
                  {
                      link->QueueRead(kShortEcuInit);
                  }
                  return link;
              },
              [] { return std::make_unique<FakeClock>(); },
              [this](LogLevel, const QString& message) { logs_.push_back(message); })
    {
        launcher_.setCompletionHandler([this](IdentifyGeneration generation, IdentifyOutcome outcome)
                                       { completions_.push_back({generation, std::move(outcome)}); });
    }

    bool waitForCompletions(std::size_t count)
    {
        return fastecu::testing::WaitUntil([&] { return completions_.size() >= count; },
                                           std::chrono::milliseconds(5000));
    }

    bool answer_ecu_ = true;
    std::vector<Completion> completions_;
    std::vector<QString> logs_;
    QtIdentifyLauncher launcher_;
};

TEST_F(QtIdentifyLauncherTest, ForwardsTheOutcomeTaggedWithTheStartGeneration)
{
    launcher_.start(SsmIdentifyRequest{}, 7);
    ASSERT_TRUE(waitForCompletions(1));

    EXPECT_EQ(completions_.at(0).generation, 7U);
    EXPECT_TRUE(completions_.at(0).outcome.success);
    EXPECT_EQ(completions_.at(0).outcome.ecu_id, "3152584006");
    EXPECT_EQ(completions_.at(0).outcome.init_response.size(), 14U);
}

TEST_F(QtIdentifyLauncherTest, ReportsAFailureWithItsDetail)
{
    answer_ecu_ = false;
    launcher_.start(SsmIdentifyRequest{}, 1);
    ASSERT_TRUE(waitForCompletions(1));

    EXPECT_FALSE(completions_.at(0).outcome.success);
    EXPECT_FALSE(completions_.at(0).outcome.error_detail.empty());
    EXPECT_FALSE(logs_.empty()); // each failed attempt is logged
}

// The coordinator's fence depends on this: a completion queued before
// stop_and_join() is still delivered afterwards, tagged with its own run.
TEST_F(QtIdentifyLauncherTest, AJoinedWorkersCompletionKeepsItsOwnGeneration)
{
    launcher_.start(SsmIdentifyRequest{}, 7);
    ASSERT_TRUE(launcher_.waitForWorker(std::chrono::milliseconds(5000)));
    launcher_.stopAndJoin();
    launcher_.start(SsmIdentifyRequest{}, 8);
    ASSERT_TRUE(waitForCompletions(2));

    EXPECT_EQ(completions_.at(0).generation, 7U);
    EXPECT_EQ(completions_.at(1).generation, 8U);
}

TEST_F(QtIdentifyLauncherTest, StopAndJoinWithoutARunIsANoOp)
{
    launcher_.stopAndJoin();
    EXPECT_TRUE(launcher_.waitForWorker(std::chrono::milliseconds(1)));
    EXPECT_TRUE(completions_.empty());
}

} // namespace

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
