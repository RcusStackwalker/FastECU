#include <QTest>
#include <QTreeWidget>

#include "src/backend/config/testing/config_session_fixture.h"
#define private public
#include "src/ui/desktop/protocol_select.h"
#undef private
#include "ui_protocol_select.h"

using fastecu::config::testing::ConfigSessionFixture;

class ProtocolSelectTest : public QObject
{
    Q_OBJECT

  private slots:
    void listsEachVehicleBackedProtocolOnce()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        ProtocolSelect dialog{f.session};
        // proto_a (two rows), proto_b, missing_proto -- still listed, as today.
        QCOMPARE(dialog.ui->treeWidget->topLevelItemCount(), 3);
    }

    void choosingRecordsTheProtocolName()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        const auto before = f.session.settings();
        ProtocolSelect dialog{f.session};
        const auto items = dialog.ui->treeWidget->findItems("proto_b", Qt::MatchExactly, 0);
        QCOMPARE(items.size(), 1);
        dialog.ui->treeWidget->setCurrentItem(items.front());
        items.front()->setSelected(true);

        QVERIFY(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

        QCOMPARE(dialog.chosen_protocol_name(), std::optional<std::string>("proto_b"));
        QVERIFY(f.session.settings() == before);
    }

    void rejectingLeavesNoChoice()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        ProtocolSelect dialog{f.session};
        dialog.reject();
        QVERIFY(!dialog.chosen_protocol_name().has_value());
    }
};

QTEST_MAIN(ProtocolSelectTest)
#include "protocol_select_test.moc"
