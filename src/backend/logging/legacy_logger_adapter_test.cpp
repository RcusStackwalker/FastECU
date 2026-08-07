#include "src/backend/logging/legacy_logger_adapter.h"

#include <QtTest>

#include "src/backend/logging/logger_definition_model.h"

class LegacyLoggerAdapterTest : public QObject
{
    Q_OBJECT

  private slots:
    void definition_fans_out_into_parallel_arrays()
    {
        fastecu::logging::LoggerDefinition definition;
        fastecu::logging::LoggerParameter p;
        p.protocol = "SSM";
        p.id = "P1";
        p.name = "Engine Speed";
        p.description = "RPM";
        p.address = "0x1234";
        p.length = "2";
        p.ecu_byte_index = "3";
        p.ecu_bit = "4";
        p.target = "1";
        p.enabled = true;
        p.conversions.push_back({"rpm", "x*0.25", "0.00", "0", "8000", "500"});
        definition.parameters.push_back(p);

        // Second parameter with two conversions: exercises the `i > 0`
        // separator branch in the packed-string loop, which a single
        // conversion never reaches.
        fastecu::logging::LoggerParameter p2;
        p2.protocol = "SSM";
        p2.id = "P2";
        p2.name = "Coolant Temp";
        p2.description = "Deg C";
        p2.address = "0x1235";
        p2.length = "1";
        p2.ecu_byte_index = "5";
        p2.ecu_bit = "0";
        p2.target = "1";
        p2.enabled = false;
        p2.conversions.push_back({"C", "x-40", "0.0", "-40", "215", "5"});
        p2.conversions.push_back({"F", "x*1.8-40", "0.0", "-40", "419", "9"});
        definition.parameters.push_back(p2);

        fastecu::logging::LoggerSwitch s;
        s.protocol = "SSM";
        s.id = "S1";
        s.name = "Test Switch";
        s.address = "0x20";
        s.ecu_bit = "1";
        definition.switches.push_back(s);

        fastecu::definitions::LogValuesStructure values;
        fastecu::logging::apply_definition(definition, values);

        QCOMPARE(values.log_value_id.size(), 2);
        QCOMPARE(values.log_value_protocol.at(0), QString("SSM"));
        QCOMPARE(values.log_value_name.at(0), QString("Engine Speed"));
        QCOMPARE(values.log_value_address.at(0), QString("0x1234"));
        QCOMPARE(values.log_value_length.at(0), QString("2"));
        QCOMPARE(values.log_value_enabled.at(0), QString("1"));
        QCOMPARE(values.log_value.at(0), QString("0.00"));
        QCOMPARE(values.log_switch_id.at(0), QString("S1"));
        QCOMPARE(values.log_switch_address.at(0), QString("0x20"));
        QCOMPARE(values.log_switch_state.at(0), QString("0"));
        // log_value_conversions is the one piece of non-trivial logic in the
        // adapter -- pin every field, in order, for both the single-
        // conversion case and the multi-conversion (two-element) case that
        // used to exercise the packed-string separator branch.
        QCOMPARE(values.log_value_conversions.at(0).size(), 1);
        QCOMPARE(values.log_value_conversions.at(0).at(0),
                 (fastecu::logging::Conversion{"rpm", "x*0.25", "0.00", "0", "8000", "500"}));
        QCOMPARE(values.log_value_conversions.at(1).size(), 2);
        QCOMPARE(values.log_value_conversions.at(1).at(0),
                 (fastecu::logging::Conversion{"C", "x-40", "0.0", "-40", "215", "5"}));
        QCOMPARE(values.log_value_conversions.at(1).at(1),
                 (fastecu::logging::Conversion{"F", "x*1.8-40", "0.0", "-40", "419", "9"}));
        // Every parallel array has the same length -- the alignment fix.
        QCOMPARE(values.log_value_protocol.size(), values.log_value_id.size());
        QCOMPARE(values.log_value_address.size(), values.log_value_id.size());
        QCOMPARE(values.log_value_length.size(), values.log_value_id.size());
    }

    // The ownership contract from the 5d-5 design doc: applying a selection
    // must never rewrite definition state. log_value_enabled is overwritten at
    // runtime from the ECU's capability response; re-fanning would reset it.
    void selection_round_trip_leaves_enabled_flags_untouched()
    {
        fastecu::definitions::LogValuesStructure values;
        // P3/S2 appear in no selection list at all -- if apply_selection
        // derived the enabled flags from the selection (writing "1" for
        // selected rows and "0" for the rest) rather than leaving definition
        // state untouched, these two would flip to "0" even though nothing
        // here says to deselect them. A test that only seeds selected rows
        // cannot tell that mistake apart from the real contract.
        values.log_value_id << "P1" << "P2" << "P3";
        values.log_value_enabled << "0" << "0" << "0";
        values.log_switch_id << "S1" << "S2";
        values.log_switch_enabled << "0" << "0";

        // Simulate the runtime capability override: the ECU reports every
        // row as enabled, including P3/S2, which the user has not selected
        // for display anywhere.
        values.log_value_enabled[0] = "1";
        values.log_value_enabled[2] = "1";
        values.log_switch_enabled[0] = "1";
        values.log_switch_enabled[1] = "1";

        fastecu::logging::LoggerSelection selection;
        selection.protocol = "SSM";
        selection.gauge_ids = {"P1"};
        selection.lower_panel_ids = {"P2"};
        selection.switch_ids = {"S1"};
        fastecu::logging::apply_selection(selection, values);

        QCOMPARE(values.log_value_enabled.at(0), QString("1"));
        QCOMPARE(values.log_value_enabled.at(2), QString("1"));
        QCOMPARE(values.log_switch_enabled.at(0), QString("1"));
        QCOMPARE(values.log_switch_enabled.at(1), QString("1"));
        QCOMPARE(values.log_value_id.size(), 3);
        QCOMPARE(values.logging_values_protocol, QString("SSM"));
        QCOMPARE(values.dashboard_log_value_id, QStringList{"P1"});
    }
};

QTEST_APPLESS_MAIN(LegacyLoggerAdapterTest)
#include "legacy_logger_adapter_test.moc"
