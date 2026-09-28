#include <QTest>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/ui/desktop/vehicle_select.h"

using fastecu::config::testing::ConfigSessionFixture;

class VehicleSelectTest : public QObject
{
    Q_OBJECT

  private slots:
    void choosingRecordsTheRowWithoutTouchingTheSession()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        QVERIFY(f.session.select_row(2).has_value());
        const auto before = f.session.settings();

        VehicleSelect dialog{f.session}; // opens on the session's row (Subaru Forester)
        QVERIFY(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.chosen_row(), std::optional<std::size_t>(2));
        QVERIFY(f.session.settings() == before);
    }

    void rejectingLeavesNoChoice()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        VehicleSelect dialog{f.session};
        dialog.reject();
        QVERIFY(!dialog.chosen_row().has_value());
    }
};

QTEST_MAIN(VehicleSelectTest)
#include "vehicle_select_test.moc"
