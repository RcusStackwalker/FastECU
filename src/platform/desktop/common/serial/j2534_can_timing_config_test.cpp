#include <cstdio>
#include <vector>

#include <QCoreApplication>
#include <QtTest>

#include "j2534_can_timing_config.h"

class TestJ2534CanTimingConfig : public QObject
{
    Q_OBJECT

  private slots:
    void iso15765_usesFastestFlowControl();
    void iso15765_retriesWithCompatibilityValues();
    void iso15765_returnsFailureAfterRetryFails();
    void rawCan_onlyDisablesLoopback();
    void rawCan_failureDoesNotRetry();
};

void TestJ2534CanTimingConfig::iso15765_usesFastestFlowControl()
{
    std::vector<std::vector<SCONFIG>> calls;
    const bool configured = configureJ2534CanTimings(
        true, [&calls](const SCONFIG_LIST& list)
        {
            calls.emplace_back(list.ConfigPtr, list.ConfigPtr + list.NumOfParams);
            return STATUS_NOERROR; });

    QVERIFY(configured);
    QCOMPARE(calls.size(), std::size_t{1});
    QCOMPARE(calls[0].size(), std::size_t{3});
    QCOMPARE(calls[0][0].Parameter, static_cast<unsigned long>(LOOPBACK));
    QCOMPARE(calls[0][0].Value, 0UL);
    QCOMPARE(calls[0][1].Parameter, static_cast<unsigned long>(ISO15765_STMIN));
    QCOMPARE(calls[0][1].Value, 0UL);
    QCOMPARE(calls[0][2].Parameter, static_cast<unsigned long>(ISO15765_BS));
    QCOMPARE(calls[0][2].Value, 0UL);
}

void TestJ2534CanTimingConfig::iso15765_retriesWithCompatibilityValues()
{
    std::vector<std::vector<SCONFIG>> calls;
    const bool configured = configureJ2534CanTimings(
        true, [&calls](const SCONFIG_LIST& list)
        {
            calls.emplace_back(list.ConfigPtr, list.ConfigPtr + list.NumOfParams);
            return calls.size() == 1 ? ERR_FAILED : STATUS_NOERROR; });

    QVERIFY(configured);
    QCOMPARE(calls.size(), std::size_t{2});
    QCOMPARE(calls[1].size(), std::size_t{3});
    QCOMPARE(calls[1][0].Parameter, static_cast<unsigned long>(LOOPBACK));
    QCOMPARE(calls[1][0].Value, 0UL);
    QCOMPARE(calls[1][1].Parameter, static_cast<unsigned long>(ISO15765_STMIN));
    QCOMPARE(calls[1][1].Value, 1UL);
    QCOMPARE(calls[1][2].Parameter, static_cast<unsigned long>(ISO15765_BS));
    QCOMPARE(calls[1][2].Value, 16UL);
}

void TestJ2534CanTimingConfig::iso15765_returnsFailureAfterRetryFails()
{
    std::size_t calls = 0;
    const bool configured = configureJ2534CanTimings(
        true, [&calls](const SCONFIG_LIST&)
        {
            ++calls;
            return ERR_FAILED; });

    QVERIFY(!configured);
    QCOMPARE(calls, std::size_t{2});
}

void TestJ2534CanTimingConfig::rawCan_onlyDisablesLoopback()
{
    std::vector<std::vector<SCONFIG>> calls;
    const bool configured = configureJ2534CanTimings(
        false, [&calls](const SCONFIG_LIST& list)
        {
            calls.emplace_back(list.ConfigPtr, list.ConfigPtr + list.NumOfParams);
            return STATUS_NOERROR; });

    QVERIFY(configured);
    QCOMPARE(calls.size(), std::size_t{1});
    QCOMPARE(calls[0].size(), std::size_t{1});
    QCOMPARE(calls[0][0].Parameter, static_cast<unsigned long>(LOOPBACK));
    QCOMPARE(calls[0][0].Value, 0UL);
}

void TestJ2534CanTimingConfig::rawCan_failureDoesNotRetry()
{
    std::size_t calls = 0;
    const bool configured = configureJ2534CanTimings(
        false, [&calls](const SCONFIG_LIST&)
        {
            ++calls;
            return ERR_FAILED; });

    QVERIFY(!configured);
    QCOMPARE(calls, std::size_t{1});
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    TestJ2534CanTimingConfig test;
    return QTest::qExec(&test, argc, argv);
}

#include "j2534_can_timing_config_test.moc"
