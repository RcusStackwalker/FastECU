#include "src/ui/desktop/biu/biu_operations_subaru.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>

#include <QStringList>
#include <gtest/gtest.h>

#include "src/backend/protocol/testing/fake_diagnostic_link.h"
#include "src/platform/desktop/common/testing/widgets_application_environment.h"

// BIU response decoding, from a received frame to the result rows each
// window shows.
class BiuOperationsSubaruTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        biu_ = std::make_unique<BiuOperationsSubaru>(link_);
        QObject::connect(biu_.get(), &BiuOperationsSubaru::logI,
                         [this](const QString& message, bool, bool) { logs_.append(message); });
    }

    // A response frame: [0x80 | length] 0xF0 0x40 <payload> <checksum>.
    static QByteArray frame(std::initializer_list<std::uint8_t> payload)
    {
        QByteArray message;
        message.append(static_cast<char>(0x80U | payload.size()));
        message.append('\xf0');
        message.append('\x40');
        for (const auto byte : payload)
        {
            message.append(static_cast<char>(byte));
        }
        std::uint8_t sum = 0;
        for (const char byte : message)
        {
            sum = static_cast<std::uint8_t>(sum + static_cast<std::uint8_t>(byte));
        }
        message.append(static_cast<char>(sum));
        return message;
    }

    void parse(const QByteArray& message)
    {
        biu_->parseBiuMessage(message);
    }
    std::string data() const
    {
        return biu_->data_result_->join('|').toStdString();
    }
    std::string switches() const
    {
        return biu_->switch_result_->join('|').toStdString();
    }
    std::string timeTemp() const
    {
        return biu_->biu_tt_result_->toHex().toStdString();
    }
    std::string options() const
    {
        return biu_->biu_option_result_->toHex().toStdString();
    }
    std::string logs() const
    {
        return logs_.join('|').toStdString();
    }
    bool connected() const
    {
        return biu_->connection_state_ == BiuOperationsSubaru::kConnected;
    }

    fastecu::diagnostics::FakeDiagnosticLink link_;
    std::unique_ptr<BiuOperationsSubaru> biu_;
    QStringList logs_;
};

TEST_F(BiuOperationsSubaruTest, rejectsMalformedFrames)
{
    parse({});
    QByteArray badHeader = frame({0xc1});
    badHeader[1] = '\x10';
    parse(badHeader);
    QByteArray badLength = frame({0xc1});
    badLength[0] = '\x82';
    parse(badLength);
    QByteArray badChecksum = frame({0xc1});
    badChecksum[badChecksum.size() - 1] = '\x00';
    parse(badChecksum);
    EXPECT_EQ(logs(), "Invalid message received: zero length|Invalid message received: invalid header|Invalid message "
                      "received: invalid length|Invalid message received: invalid checksum");
    EXPECT_FALSE(connected());
}

TEST_F(BiuOperationsSubaruTest, connectsDisconnectsAndReportsErrors)
{
    parse(frame({0xc1, 0x8f, 0xea}));
    EXPECT_TRUE(connected());
    parse(frame({0xc2}));
    EXPECT_FALSE(connected());
    parse(frame({0x54, 0x40, 0x00}));
    parse(frame({0x7f, 0x21, 0x12}));
    EXPECT_EQ(logs(), "Connection to BIU successful|Disconnection from BIU successful|BIU DTCs successfully "
                      "cleared|Error response received from BIU");
}

TEST_F(BiuOperationsSubaruTest, decodesDtcs)
{
    parse(frame({0x58, 0x00, 0x01, 0x00, 0x01, 0x0a, 0x99, 0x00}));
    EXPECT_EQ(data(), "B0100 - BIU internal error (stored)|B0a99 (pending)");
    parse(frame({0x58, 0x00}));
    EXPECT_EQ(data(), "No BIU DTC found");
}

TEST_F(BiuOperationsSubaruTest, decodesSwitchBits)
{
    parse(frame({0x61, 0x50, 0x81, 0x02}));
    EXPECT_EQ(switches(),
              "Key-lock warning SW                  |ON|Stop Light Switch                    |OFF|Front fog lamp SW "
              "input              |OFF|Rear fog lamp SW input               |OFF|Lighting SW input                    "
              "|OFF|Door key-lock SW input               |OFF|Door unlock SW input                 |OFF|---- unused "
              "----                     |ON|Driver\xE2\x80\x99s door SW input               |OFF|P-door SW input       "
              "               |ON|Rear right door SW input             |OFF|Rear left door SW input              "
              "|OFF|R Gate SW input                      |OFF|Manual lock SW input                 |OFF|Manual unlock "
              "SW input               |OFF|Lock SW (guess)                      |OFF");
    parse(frame({0x61, 0x51, 0x05}));
    EXPECT_EQ(switches(), "Lighting I sw input    |ON|Lighting II sw input   |OFF|---- unused ----       |ON|Dimmer hi "
                          "sw input     |OFF|---- unused ----       |OFF|Dimmer pass sw input   |OFF|---- unused ----  "
                          "     |OFF|---- unused ----       |OFF");
}

TEST_F(BiuOperationsSubaruTest, decodesOptionBits)
{
    parse(frame({0x61, 0x53, 0x81, 0x40}));
    EXPECT_EQ(
        switches(),
        "Rear defogger op mode       |NORMAL|Wiper deicer op mode        |CONTINUOUS|Security alarm setup        "
        "|OFF|Impact sensor setup         |OFF|Alarm monitor delay setting |0s DELAY|Lockout prevention          "
        "|OFF|Impact sensor               |NO|Siren installed             |YES|Answer back buzzer setup    |OFF|Hazard "
        "answer back setup    |OFF|Automatic locking setup     |OFF|Ans-back buzzer             |NO|Auto locking       "
        "         |NO|Initial keyless setting     |-|Initial button setting      |EXEC|Initial security setting    |-");
    EXPECT_EQ(options(), "8140");
}

TEST_F(BiuOperationsSubaruTest, scalesBiuData)
{
    parse(frame({0x61, 0x40, 0x9a, 0x9b, 0x10, 0x50}));
    EXPECT_EQ(data(), "Battery voltage (control) 12.9822 volts|Battery voltage (backup)  13.0665 volts|Ignition system "
                      "voltage   1.3488 volts|Accessory voltage         6.744 volts");
}

TEST_F(BiuOperationsSubaruTest, scalesCanData)
{
    parse(frame({0x61, 0x41, 0x10, 0x27, 0x34, 0x12, 0x05, 0xe8, 0x03, 0x64, 0x00, 0x5a, 0x08, 0x02, 0x03}));
    EXPECT_EQ(data(),
              "Front wheel speed     562 km/hr|VDC/ABS latest f-code 12 34      |Blower fan steps      5 steps|Fuel "
              "level resistance 1.6 ohms |Fuel consumption      0.1 cc/s |Engine coolant temp   50 degC |Longitudinal "
              "g-force  0.988 m/s^2|Sport shift stages    2 step |Shift position        3      ");
}

TEST_F(BiuOperationsSubaruTest, decodesTimesAndTemperatures)
{
    parse(frame({0x61, 0x52, 0x02, 0x03}));
    EXPECT_EQ(data(), "Room lamp off delay time Short     |Auto-lock time           30 secs ");
    parse(frame({0x61, 0x52, 0x13, 0x05, 0x0e}));
    EXPECT_EQ(data(),
              "Room lamp off delay time Long     |Auto-lock time           50 secs |Outside temp offset      -1 degC ");
    EXPECT_EQ(timeTemp(), "020303050e");
}

TEST_F(BiuOperationsSubaruTest, decodesStatusBytes)
{
    parse(frame({0x61, 0x60, 0x0d}));
    EXPECT_EQ(data(), "VDC/ABS Condition: 5");
    parse(frame({0x61, 0x61, 0xf3, 0xc5}));
    EXPECT_EQ(data(), "Destination:    3|Touchscreen SW: 5");
    parse(frame({0x61, 0x54, 0x01}));
    EXPECT_EQ(data(), "Factory Initial Setting: Factory");
    parse(frame({0x61, 0x54, 0x00}));
    EXPECT_EQ(data(), "Factory Initial Setting: Market");
}

namespace
{
const auto *const kEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
