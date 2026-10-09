#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <gtest/gtest.h>
#include <QTreeWidget>

#include "src/backend/config/testing/config_session_fixture.h"
#define private public
#include "src/ui/desktop/widgets/protocol_select.h"
#undef private
#include "ui_protocol_select.h"

using fastecu::config::testing::ConfigSessionFixture;

TEST(ProtocolSelectTest, listsEachVehicleBackedProtocolOnce)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    ProtocolSelect dialog{f.session};
    // proto_a (two rows) and proto_b.
    ASSERT_EQ(dialog.ui->treeWidget->topLevelItemCount(), 2);
}

TEST(ProtocolSelectTest, choosingRecordsTheProtocolName)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    const auto before = f.session.settings();
    ProtocolSelect dialog{f.session};
    const auto items = dialog.ui->treeWidget->findItems("proto_b", Qt::MatchExactly, 0);
    ASSERT_EQ(items.size(), 1);
    dialog.ui->treeWidget->setCurrentItem(items.front());
    items.front()->setSelected(true);

    ASSERT_TRUE(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

    ASSERT_EQ(dialog.chosen_protocol_name(), std::optional<std::string>("proto_b"));
    ASSERT_TRUE(f.session.settings() == before);
}

TEST(ProtocolSelectTest, rejectingLeavesNoChoice)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    ProtocolSelect dialog{f.session};
    dialog.reject();
    ASSERT_TRUE(!dialog.chosen_protocol_name().has_value());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
