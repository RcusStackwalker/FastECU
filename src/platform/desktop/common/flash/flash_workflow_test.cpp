#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QtTest>

namespace fastecu::flash
{
namespace
{

FlashWorkflowRequest request(std::string protocol, FlashOperation operation = FlashOperation::Read)
{
    return {.operation = operation,
            .protocol = std::move(protocol),
            .mcu = "M32R_384KB_1block",
            .image = std::nullopt,
            .paths = {},
            .display_filename = "test.bin",
            .serial = nullptr};
}

class FlashWorkflowTest : public QObject
{
    Q_OBJECT
  private slots:
    void recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone();
    void invalidColtSuffixIsRecognizedButFailsPreflight();
    void preflightPrecedesPromptsAndDeclineCancels();
    void successfulReadBytesAreAcceptedAutomatically();
    void coltWriteUsesColtSpecificSafetyPrompts();
};

void FlashWorkflowTest::recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone()
{
    const char *portable[] = {
        "mitsu_ecu_m32r_can", "mitsu_ecu_m32r_can_vendor_ext",
        "mitsu_ecu_m32r_can_512kb", "mitsu_ecu_m32r_can_vendor_ext_512kb",
        "sub_ecu_eeprom_denso_sh7055_kline", "sub_ecu_eeprom_denso_sh7058_kline",
        "sub_ecu_eeprom_denso_sh7055_densocan", "sub_ecu_eeprom_denso_sh7058_densocan",
        "sub_ecu_eeprom_denso_sh7058_can", "sub_ecu_eeprom_denso_sh7058_can_diesel"};
    for (const char *protocol : portable)
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    QVERIFY(FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_m32r_can")) == nullptr);
}

void FlashWorkflowTest::invalidColtSuffixIsRecognizedButFailsPreflight()
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can_typo"));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::preflightPrecedesPromptsAndDeclineCancels()
{
    auto invalid = request("mitsu_ecu_m32r_can", FlashOperation::TestWrite);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(invalid));
    QVERIFY(std::holds_alternative<FlashFailureStep>(workflow->next()));

    workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::successfulReadBytesAreAcceptedAutomatically()
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    (void)workflow->next();
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2, 3}});
    auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({1, 2, 3}));
}

void FlashWorkflowTest::coltWriteUsesColtSpecificSafetyPrompts()
{
    auto write = request("mitsu_ecu_m32r_can", FlashOperation::Write);
    write.image = bytes::Bytes(0x60000);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(write));

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind,
             FlashPromptKind::ColtEraseTrigger);
}

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashWorkflowTest)
#include "flash_workflow_test.moc"
