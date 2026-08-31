#include "src/platform/desktop/common/connection/j2534_adapter_provider.h"

#include <QTest>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace fastecu::desktop::connection
{

class J2534AdapterProviderTestAccess
{
  public:
    using Dependencies = J2534AdapterProvider::Dependencies;

    static std::unique_ptr<J2534AdapterProvider> make(Dependencies dependencies)
    {
        return std::unique_ptr<J2534AdapterProvider>(
            new J2534AdapterProvider(std::move(dependencies), J2534AdapterProvider::TestingTag{}));
    }
};

namespace
{

class CountedFakeBackend final : public FakeBackend
{
  public:
    explicit CountedFakeBackend(std::atomic<int> *destruction_count) : destruction_count_(destruction_count)
    {
    }

    ~CountedFakeBackend() override
    {
        destruction_count_->fetch_add(1);
    }

  private:
    std::atomic<int> *destruction_count_;
};

dashboard::CdbgConnectionProfile profile()
{
    return {
        .bitrate = 250000,
        .identifier_width = dashboard::CanIdentifierWidth::Extended,
        .request_id = 0x18daf101,
        .reply_id = 0x18daf110,
        .stream_instance = 2,
        .sampling_interval_ms = 50,
        .retry = {.poll_timeout_ms = 20, .silence_threshold = 3, .reconnect_attempts = 1, .reconnect_period_ms = 100},
    };
}

class ProviderHarness
{
  public:
    QStringList listed_entries;
    detail::J2534DiscoverySource discovery_source = detail::J2534DiscoverySource::UnixSerialPorts;
    fastecu::Status configure_result;
    J2534RawCanOpenResult open_result{
        .opened_port = "opened",
        .failure = J2534RawCanOpenFailure::None,
        .stage = J2534RawCanOpenStage::None,
        .api_status = 0,
    };
    bool list_throws = false;
    bool configure_throws = false;
    bool open_throws = false;
    std::optional<logging::RawCanSetupProfile> configured_profile;
    QStringList selected_entries;
    std::atomic<int> destructions{0};

    std::unique_ptr<J2534AdapterProvider> provider()
    {
        J2534AdapterProviderTestAccess::Dependencies dependencies{
            .discovery_source = discovery_source,
            .list = [this](SerialPortActions&) -> QStringList
            {
                if (list_throws)
                {
                    throw std::runtime_error("list failed");
                }
                return listed_entries;
            },
            .construct =
                [this]
            {
                auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr, [this]() -> SerialBackend *
                                                                  { return new CountedFakeBackend(&destructions); });
                serial->set_add_ssm_header(false); // materialize the backend so its lifetime is observable
                return serial;
            },
            .configure = [this](SerialPortActions& serial,
                                const logging::RawCanSetupProfile& raw_profile) -> fastecu::Status
            {
                selected_entries = serial.get_serial_port_list();
                configured_profile = raw_profile;
                if (configure_throws)
                {
                    throw std::runtime_error("configure failed");
                }
                return configure_result;
            },
            .open = [this](SerialPortActions&) -> J2534RawCanOpenResult
            {
                if (open_throws)
                {
                    throw std::runtime_error("open failed");
                }
                return open_result;
            },
        };
        return J2534AdapterProviderTestAccess::make(std::move(dependencies));
    }
};

void expect_disconnected(const fastecu::Result<std::unique_ptr<OpenedCanAdapter>>& result)
{
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, fastecu::ErrorKind::Disconnected);
}

} // namespace

class TestJ2534AdapterProvider : public QObject
{
    Q_OBJECT

  private slots:
    void unixDiscoveryIncludesOnlyOpenPortEntriesWithStablePresentation()
    {
        ProviderHarness harness;
        harness.listed_entries = {
            "ttyUSB0 - USB Serial",        "COM3 - USB Serial", "wss://remote.example/Remote J2534",
            "cu.usbmodem0 - OpenPort 2.0", "Acme J2534 DLL",
        };
        auto provider = harness.provider();

        const auto discovered = provider->discover();

        QVERIFY(discovered.has_value());
        QCOMPARE(discovered->size(), 1U);
        const auto& openport = (*discovered)[0];
        QCOMPARE(openport.kind, dashboard::AdapterKind::J2534);
        QCOMPARE(openport.vendor, std::string("Tactrix"));
        QCOMPARE(openport.display_name, std::string("OpenPort 2.0"));
        QCOMPARE(openport.label, std::string("Tactrix OpenPort 2.0"));
        QVERIFY(openport.candidate_id.starts_with("j2534:"));
        QVERIFY(openport.candidate_id.find("usbmodem") == std::string::npos);

        QCOMPARE(harness.destructions.load(), 1);

        const auto rediscovered = provider->discover();
        QVERIFY(rediscovered.has_value());
        QCOMPARE(rediscovered->size(), 1U);
        QCOMPARE(rediscovered->front().candidate_id, openport.candidate_id);
        QCOMPARE(harness.destructions.load(), 2);
    }

    void windowsDiscoveryIncludesUnmarkedRegistryNamesButExcludesSerialAndRemoteEntries()
    {
        ProviderHarness harness;
        harness.discovery_source = detail::J2534DiscoverySource::WindowsRegistryAndSerialPorts;
        harness.listed_entries = {
            "Shared Vendor",
            "Acme J2534 DLL",
            "COM3 - USB Serial",
            "ttyUSB0 - USB Serial",
            "wss://remote.example/Remote J2534",
            "",
        };
        auto provider = harness.provider();

        const auto discovered = provider->discover();

        QVERIFY(discovered.has_value());
        QCOMPARE(discovered->size(), 2U);
        QCOMPARE((*discovered)[0].vendor, std::string("Shared Vendor"));
        QCOMPARE((*discovered)[0].display_name, std::string("J2534"));
        QCOMPARE((*discovered)[1].vendor, std::string("Acme"));
    }

    void opensOnlyAnIssuedCandidateAndAppliesTheDocumentProfile()
    {
        ProviderHarness harness;
        const QString selected = "cu.usbmodem0 - OpenPort 2.0";
        harness.listed_entries = {selected};
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        QCOMPARE(discovered->size(), 1U);

        auto opened = provider->open(discovered->front().candidate_id, profile());

        QVERIFY(opened.has_value());
        QCOMPARE(harness.selected_entries, QStringList({selected}));
        QVERIFY(harness.configured_profile.has_value());
        QCOMPARE(harness.configured_profile->bitrate, 250000U);
        QCOMPARE(harness.configured_profile->identifier_width, dashboard::CanIdentifierWidth::Extended);
        QCOMPARE(harness.configured_profile->reply_id, 0x18daf110U);
        QCOMPARE(harness.destructions.load(), 1);

        auto transport = std::move(**opened).into_transport();
        QVERIFY(transport->isOpen());
        transport.reset();
        QCOMPARE(harness.destructions.load(), 2);
    }

    void rejectsCandidateIdsNotIssuedByThisProviderWithoutConstructingAnAdapter()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        auto provider = harness.provider();
        QVERIFY(provider->discover().has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open("socketcan:can0", profile());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::InvalidConfig);
        QCOMPARE(harness.destructions.load(), after_discovery);
    }

    void emptyOpenResultReturnsDisconnectedAndDestroysTheCreatedActions()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        harness.open_result = {
            .opened_port = {},
            .failure = J2534RawCanOpenFailure::AdapterUnavailable,
            .stage = J2534RawCanOpenStage::DeviceOpen,
            .api_status = 8,
        };
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open(discovered->front().candidate_id, profile());

        expect_disconnected(result);
        QCOMPARE(harness.destructions.load(), after_discovery + 1);
    }

    void unsupportedConnectFailureIsTypedAndDestroysTheCreatedActions()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        harness.open_result = {
            .opened_port = {},
            .failure = J2534RawCanOpenFailure::UnsupportedConfiguration,
            .stage = J2534RawCanOpenStage::ChannelConnect,
            .api_status = 0x19,
        };
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open(discovered->front().candidate_id, profile());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::Unsupported);
        QVERIFY(QString::fromStdString(result.error().detail).contains("connect"));
        QCOMPARE(harness.destructions.load(), after_discovery + 1);
    }

    void timingFailureIsInternalAndDestroysTheCreatedActions()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        harness.open_result = {
            .opened_port = {},
            .failure = J2534RawCanOpenFailure::Internal,
            .stage = J2534RawCanOpenStage::TimingConfiguration,
            .api_status = 7,
        };
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open(discovered->front().candidate_id, profile());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::Internal);
        QVERIFY(QString::fromStdString(result.error().detail).contains("timing"));
        QCOMPARE(harness.destructions.load(), after_discovery + 1);
    }

    void filterFailureIsDisconnectedAndDestroysTheCreatedActions()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        harness.open_result = {
            .opened_port = {},
            .failure = J2534RawCanOpenFailure::AdapterUnavailable,
            .stage = J2534RawCanOpenStage::FilterConfiguration,
            .api_status = 8,
        };
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open(discovered->front().candidate_id, profile());

        expect_disconnected(result);
        QVERIFY(QString::fromStdString(result.error().detail).contains("filter"));
        QCOMPARE(harness.destructions.load(), after_discovery + 1);
    }

    void configurationFailureDestroysTheCreatedActions()
    {
        ProviderHarness harness;
        harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        harness.configure_result = fastecu::fail(fastecu::ErrorKind::InvalidConfig, "setup failed");
        auto provider = harness.provider();
        const auto discovered = provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = harness.destructions.load();

        const auto result = provider->open(discovered->front().candidate_id, profile());

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, fastecu::ErrorKind::InvalidConfig);
        QCOMPARE(harness.destructions.load(), after_discovery + 1);
    }

    void thrownDiscoveryAndOpenActionsReleaseAnyConstructedActions()
    {
        ProviderHarness discovery_harness;
        discovery_harness.list_throws = true;
        auto discovery_provider = discovery_harness.provider();
        const auto discovery = discovery_provider->discover();
        QVERIFY(!discovery.has_value());
        QCOMPARE(discovery.error().kind, fastecu::ErrorKind::Internal);
        QCOMPARE(discovery_harness.destructions.load(), 1);

        ProviderHarness open_harness;
        open_harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        open_harness.open_throws = true;
        auto open_provider = open_harness.provider();
        const auto discovered = open_provider->discover();
        QVERIFY(discovered.has_value());
        const int after_discovery = open_harness.destructions.load();
        const auto opened = open_provider->open(discovered->front().candidate_id, profile());
        QVERIFY(!opened.has_value());
        QCOMPARE(opened.error().kind, fastecu::ErrorKind::Internal);
        QCOMPARE(open_harness.destructions.load(), after_discovery + 1);
    }

    void thrownConfigurationActionReleasesTheCreatedActions()
    {
        ProviderHarness configure_harness;
        configure_harness.listed_entries = {"cu.usbmodem0 - OpenPort 2.0"};
        configure_harness.configure_throws = true;
        auto configure_provider = configure_harness.provider();
        const auto configure_candidates = configure_provider->discover();
        QVERIFY(configure_candidates.has_value());
        const int after_configure_discovery = configure_harness.destructions.load();
        const auto configure_result = configure_provider->open(configure_candidates->front().candidate_id, profile());
        QVERIFY(!configure_result.has_value());
        QCOMPARE(configure_result.error().kind, fastecu::ErrorKind::Internal);
        QCOMPARE(configure_harness.destructions.load(), after_configure_discovery + 1);
    }
};

} // namespace fastecu::desktop::connection

QTEST_MAIN(fastecu::desktop::connection::TestJ2534AdapterProvider)
#include "j2534_adapter_provider_test.moc"
