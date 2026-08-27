#include "src/platform/desktop/common/transport/fastecu_can_transport.h"

#include <QTest>

#include <memory>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace
{

class CountingFakeBackend final : public FakeBackend
{
  public:
    explicit CountingFakeBackend(int *destruction_count) : destruction_count_(destruction_count)
    {
    }

    ~CountingFakeBackend() override
    {
        ++*destruction_count_;
    }

  private:
    int *destruction_count_;
};

std::unique_ptr<SerialPortActions> make_serial(FakeBackend **backend, int *destruction_count)
{
    auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                      [backend, destruction_count]() -> SerialBackend *
                                                      {
                                                          auto *created = new CountingFakeBackend(destruction_count);
                                                          *backend = created;
                                                          return created;
                                                      });
    serial->set_add_ssm_header(false); // start the backend before the test configures it
    return serial;
}

} // namespace

class TestFastEcuCanTransport : public QObject
{
    Q_OBJECT

  private slots:
    void ownedSerialDelegatesOpenStateAndIsDestroyedWithTransport()
    {
        int destruction_count = 0;
        FakeBackend *backend = nullptr;
        auto serial = make_serial(&backend, &destruction_count);
        backend->portOpen.store(false);

        {
            cdbg::FastEcuCanTransport transport(std::move(serial));
            QVERIFY(!transport.isOpen());
            backend->portOpen.store(true);
            QVERIFY(transport.isOpen());
            QCOMPARE(destruction_count, 0);
        }

        QCOMPARE(destruction_count, 1);
    }

    void borrowedSerialRemainsOwnedByLegacyCaller()
    {
        int destruction_count = 0;
        FakeBackend *backend = nullptr;
        auto serial = make_serial(&backend, &destruction_count);
        backend->portOpen.store(true);

        {
            cdbg::FastEcuCanTransport transport(serial.get());
            QVERIFY(transport.isOpen());
        }

        QCOMPARE(destruction_count, 0);
        QVERIFY(serial->is_serial_port_open());
        serial.reset();
        QCOMPARE(destruction_count, 1);
    }
};

QTEST_MAIN(TestFastEcuCanTransport)
#include "fastecu_can_transport_test.moc"
