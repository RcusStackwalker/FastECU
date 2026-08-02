// Deterministic cancel/unblock/join teardown coverage for FlashWorker (step
// 5c, Task 11). Every plan built below uses build_denso_sh705x_eeprom_plan
// with a FakeClock injected into the worker -- both are real, already-tested
// portable components (Tasks 2/6), not stand-ins invented for this suite.
// The FakeClock is what makes the "blocked read" scenario deterministic: a
// real desktop clock's cancellable-but-still-real sleeps between
// connect_bootloader()'s protocol steps would let QTest::qWait(20) race
// against them, defeating the point of proving the *transport* unblock (not
// a timing coincidence) is what makes teardown prompt.
#include "src/platform/desktop/common/flash/flash_worker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::flash::DensoSecurityVariant;
using fastecu::flash::DensoSh705xEepromInput;
using fastecu::flash::DensoSh705xEepromKlineExecutor;
using fastecu::flash::EepromReadMode;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashWorker;
using fastecu::flash::FlashWorkerResult;
using fastecu::flash::KernelImage;
using fastecu::flash::MemoryRegion;
using fastecu::flash::ScriptedKlineFlashTransport;

namespace
{

// Every field here matches an SH7055 K-Line (or CAN, for the mismatch test)
// EEPROM read plan that build_denso_sh705x_eeprom_plan/validate_and_build
// accept outright -- see src/backend/flash/eeprom/denso_sh705x_eeprom_
// common.cpp's resolve_mcu_bounds("SH7055") for the eeprom/kernel-RAM bounds
// this must satisfy (eeprom = {0, 0x100}, kernel RAM = {0xFFFF6004, 0x6000}).
DensoSh705xEepromInput validInput(FlashFamily family)
{
    return DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = family,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01}},
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    };
}

// Byte-for-byte transcription of the anonymous-namespace
// request_kernel_id_frame() in denso_sh705x_eeprom_kline_executor.cpp: NOT
// SsmProtocol::addHeader-framed, unlike every other exchange in that file.
// connect_bootloader()'s very first action (after the bootloader-speed
// probe delay) is exactly this write, so ScriptedKlineFlashTransport must
// have it queued via expectWrite() before the ensuing read() can be reached
// at all -- otherwise the unscripted write itself fails with
// ErrorKind::Internal before the blocking read is ever attempted, which
// would prove nothing about unblock/cancellation.
bytes::Bytes requestKernelIdRequest()
{
    bytes::Bytes out{
        static_cast<bytes::Byte>((0xbeef >> 8) & 0xFF),
        static_cast<bytes::Byte>(0xbeef & 0xFF),
        0x00,
        0x01,
        0x01,
    };
    out.push_back(SsmProtocol::checksum(out, false));
    return out;
}

} // namespace

class TestFlashWorker : public QObject
{
    Q_OBJECT

  private slots:

    void closingWhileReadIsBlocked_cancelsUnblocksAndJoinsWithoutWallClockSleep()
    {
        auto plan = fastecu::flash::build_denso_sh705x_eeprom_plan(
            validInput(FlashFamily::DensoSh705xEepromKline));
        QVERIFY(plan.has_value());

        auto transport = std::make_unique<ScriptedKlineFlashTransport>();
        ScriptedKlineFlashTransport *rawTransport = transport.get();
        // connect_bootloader()'s initial kernel-alive probe: the write must
        // be scripted so it succeeds, so the ensuing read() is the one that
        // actually blocks.
        rawTransport->expectWrite(requestKernelIdRequest());
        rawTransport->queueBlockingRead();

        FlashWorker worker(*plan, std::make_unique<DensoSh705xEepromKlineExecutor>(),
                           std::move(transport), std::make_unique<FakeClock>());
        QSignalSpy finishedSpy(&worker, &FlashWorker::finished);

        worker.start();
        QTest::qWait(20); // let the worker thread reach the blocking read
        worker.requestStop();

        QElapsedTimer timer;
        timer.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(worker.wait(2000));
        // The proof this test exists for: unblock is a condition-variable
        // wakeup inside the fake, not a wall-clock wait, so teardown
        // completes in well under the 2000ms test timeout budget.
        QVERIFY(timer.elapsed() < 500);

        QCOMPARE(finishedSpy.count(), 1);
        auto result = finishedSpy.at(0).at(0).value<FlashWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
        QCOMPARE(rawTransport->close_call_count_, 1);
    }

    void oneAndOnlyOneTerminalResultIsEmitted()
    {
        // A CAN-shaped plan handed to the K-Line executor: check_family_
        // transport_match() rejects it before any I/O (zero writes/reads
        // scripted below, on purpose -- reaching the transport at all here
        // would itself be a bug).
        auto plan = fastecu::flash::build_denso_sh705x_eeprom_plan(
            validInput(FlashFamily::DensoSh705xEepromCan));
        QVERIFY(plan.has_value());

        auto transport = std::make_unique<ScriptedKlineFlashTransport>();
        FlashWorker worker(*plan, std::make_unique<DensoSh705xEepromKlineExecutor>(),
                           std::move(transport), std::make_unique<FakeClock>());
        QSignalSpy finishedSpy(&worker, &FlashWorker::finished);

        worker.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(worker.wait(2000));
        // Give any (bug-induced) second emission a chance to arrive before
        // asserting there is exactly one -- finishedSpy.wait() only proves
        // "at least one arrived by now", not "never more than one".
        QTest::qWait(50);

        QCOMPARE(finishedSpy.count(), 1);
        auto result = finishedSpy.at(0).at(0).value<FlashWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::InvalidConfig);
    }
};

QTEST_GUILESS_MAIN(TestFlashWorker)
#include "flash_worker_test.moc"
