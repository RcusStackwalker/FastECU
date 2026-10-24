#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QTreeWidget>

#include "src/backend/config/testing/config_session_fixture.h"
#define private public
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/widgets/protocol_select.h"
#undef private
#include "ui_protocol_select.h"

using fastecu::config::testing::ConfigSessionFixture;
using fastecu::testing::IsOk;
using ::testing::Eq;

TEST(ProtocolSelectTest, listsEachVehicleBackedProtocolOnce)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    ProtocolSelect dialog{f.session};
    // proto_a (two rows) and proto_b.
    ASSERT_EQ(dialog.ui_->treeWidget->topLevelItemCount(), 2);
}

TEST(ProtocolSelectTest, choosingRecordsTheProtocolName)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    const auto before = f.session.Settings();
    ProtocolSelect dialog{f.session};
    const auto items = dialog.ui_->treeWidget->findItems("proto_b", Qt::MatchExactly, 0);
    ASSERT_EQ(items.size(), 1);
    dialog.ui_->treeWidget->setCurrentItem(items.front());
    items.front()->setSelected(true);

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "carModelSelected", Qt::DirectConnection));

    ASSERT_EQ(dialog.acceptedProtocolName(), std::optional<std::string>("proto_b"));
    ASSERT_TRUE(f.session.Settings() == before);
}

TEST(ProtocolSelectTest, rejectingLeavesNoChoice)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.Initialize(), IsOk());
    ProtocolSelect dialog{f.session};
    dialog.reject();
    ASSERT_THAT(dialog.acceptedProtocolName(), Eq(std::nullopt));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
