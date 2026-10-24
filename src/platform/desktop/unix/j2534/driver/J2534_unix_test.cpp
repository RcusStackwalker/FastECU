#include "src/platform/desktop/unix/j2534/driver/J2534_unix.h"

#include <array>
#include <string_view>
#include <thread>

#include <QByteArray>
#include <gtest/gtest.h>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <poll.h>
#include <unistd.h>

#include "src/platform/desktop/common/testing/core_application_environment.h"

// Openport 2.0 serial framing as PassThruReadMsgs and PassThruIoctl parse and
// emit it, driven over a pseudo-terminal. The master end plays the adapter.
namespace
{

constexpr unsigned long kTimeoutMs = 50;

using std::string_view_literals::operator""sv;

// Wire bytes from a literal, embedded NULs included.
QByteArray Bytes(std::string_view bytes)
{
    return {bytes.data(), static_cast<qsizetype>(bytes.size())};
}

class J2534UnixTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        int slave = -1;
        std::array<char, 256> name{};
        ASSERT_EQ(openpty(&master_, &slave, name.data(), nullptr, nullptr), 0);
        const QString path = QString::fromLocal8Bit(name.data());
        ASSERT_EQ(j2534_.OpenSerialPort(path), path);
    }

    void TearDown() override
    {
        j2534_.CloseSerialPort();
        ::close(master_);
    }

    void Send(const QByteArray& bytes) const
    {
        ASSERT_EQ(::write(master_, bytes.constData(), static_cast<std::size_t>(bytes.size())), bytes.size());
    }

    // Everything the driver wrote, once it has been quiet for 100 ms.
    QByteArray Sent() const
    {
        QByteArray sent;
        pollfd fd{master_, POLLIN, 0};
        std::array<char, 256> buf{};
        while (::poll(&fd, 1, 100) > 0)
        {
            const auto n = ::read(master_, buf.data(), buf.size());
            if (n <= 0)
            {
                break;
            }
            sent.append(buf.data(), n);
        }
        return sent;
    }

    // Answers the driver's next request line with `reply`.
    std::thread Answer(const QByteArray& reply) const
    {
        return std::thread(
            [this, reply]
            {
                QByteArray request;
                std::array<char, 1> byte{};
                while (!request.endsWith('\n') && ::read(master_, byte.data(), 1) == 1)
                {
                    request.append(byte[0]);
                }
                Send(reply);
            });
    }

    QByteArray Data(const PassThruMsg& msg, qsizetype length) const
    {
        return {reinterpret_cast<const char *>(msg.data), length};
    }

    int master_ = -1;
    J2534 j2534_;
    PassThruMsg msg_{};
    unsigned long num_msgs_ = 1;
};

TEST_F(J2534UnixTest, readMsgs_timestampedFrame_copiesPayloadAndStops)
{
    Send(Bytes("ar5\x07\x00\x01\x02\x03\x04\xaa\xbb"sv) + Bytes("ar5\x05\x00\x00\x00\x00\x00"sv));

    ASSERT_EQ(j2534_.PassThruReadMsgs(0, &msg_, &num_msgs_, kTimeoutMs), kJ2534StatusNoerror);
    EXPECT_EQ(num_msgs_, 1U);
    EXPECT_EQ(msg_.rx_status, 0U);
    EXPECT_EQ(msg_.data_size, 2U);
    EXPECT_EQ(Data(msg_, 2), QByteArray("\xaa\xbb"));
    EXPECT_EQ(msg_.timestamp, 0x01020304U);

    // The next frame stays queued for the next read.
    ASSERT_EQ(j2534_.PassThruReadMsgs(0, &msg_, &num_msgs_, kTimeoutMs), kJ2534StatusNoerror);
    EXPECT_EQ(msg_.data_size, 0U);
}

TEST_F(J2534UnixTest, readMsgs_skipsAckAndTxDone)
{
    Send(QByteArray("aro\r\n") + Bytes("ar4\x01\x10"sv) + Bytes("ar5\x06\x00\x00\x00\x00\x00\x55"sv));

    ASSERT_EQ(j2534_.PassThruReadMsgs(0, &msg_, &num_msgs_, kTimeoutMs), kJ2534StatusNoerror);
    EXPECT_TRUE(j2534_.GetIsTxDone());
    EXPECT_EQ(num_msgs_, 1U);
    EXPECT_EQ(msg_.data_size, 1U);
    EXPECT_EQ(Data(msg_, 1), QByteArray("\x55"));
}

// An untimestamped frame does not end the read; it waits out the timeout and
// leaves the count and size alone.
TEST_F(J2534UnixTest, readMsgs_untimestampedFrame_readsUntilSilence)
{
    msg_.data_size = 99;
    Send(Bytes("ar4\x04\x00\xaa\xbb\xcc"sv));

    ASSERT_EQ(j2534_.PassThruReadMsgs(0, &msg_, &num_msgs_, kTimeoutMs), kJ2534StatusNoerror);
    EXPECT_EQ(num_msgs_, 0U);
    EXPECT_EQ(msg_.rx_status, 0U);
    EXPECT_EQ(msg_.data_size, 99U);
    EXPECT_EQ(Data(msg_, 3), QByteArray("\xaa\xbb\xcc"));
}

// A multi-frame message: start indication, a first chunk with one timestamp,
// then an end indication whose later chunk carries two.
TEST_F(J2534UnixTest, readMsgs_chunkedFrame_assemblesPayload)
{
    Send(Bytes("ar6\x05\x80\x00\x00\x00\x00"sv));
    Send(Bytes("ar6\x07\x00\x00\x00\x00\x00\x11\x22"sv));
    Send(Bytes("ar6\x0a\x40\x0a\x0b\x0c\x0d\x00\x00\x00\x00\x33"sv));

    ASSERT_EQ(j2534_.PassThruReadMsgs(0, &msg_, &num_msgs_, kTimeoutMs), kJ2534StatusNoerror);
    EXPECT_EQ(num_msgs_, 2U);
    EXPECT_EQ(msg_.rx_status, 0x40U);
    EXPECT_EQ(msg_.data_size, 3U);
    EXPECT_EQ(Data(msg_, 3), QByteArray("\x11\x22\x33"));
    EXPECT_EQ(msg_.timestamp, 0x0a0b0c0dU);
}

TEST_F(J2534UnixTest, readVbatt_parsesLastFieldOfReply)
{
    std::thread adapter = Answer("arr16 12345\r\n");
    unsigned long millivolts = 0;

    ASSERT_EQ(j2534_.PassThruIoctl(0, kJ2534ReadVbatt, nullptr, &millivolts), kJ2534StatusNoerror);
    adapter.join();
    EXPECT_EQ(millivolts, 12345U);
}

TEST_F(J2534UnixTest, fiveBaudInit_sendsAddressAndReturnsReplyLine)
{
    std::thread adapter = Answer("arw3 8f\r\n");
    std::array<unsigned char, 1> address{0x33};
    std::array<unsigned char, 32> reply{};
    const SByteArray input{address.size(), address.data()};
    SByteArray output{0, reply.data()};

    ASSERT_EQ(j2534_.PassThruIoctl(3, kJ2534FiveBaudInit, &input, &output), kJ2534StatusNoerror);
    adapter.join();
    EXPECT_EQ(QByteArray(reinterpret_cast<const char *>(reply.data()), static_cast<qsizetype>(output.num_of_bytes)),
              "arw3 8f\r\n");
}

TEST_F(J2534UnixTest, setConfig_sendsOneLinePerParameter)
{
    std::array<SCONFIG, 2> params{{{kJ2534DataRate, 10400}, {kJ2534Loopback, 0}}};
    const SConfigList list{params.size(), params.data()};

    ASSERT_EQ(j2534_.PassThruIoctl(3, kJ2534SetConfig, &list, nullptr), kJ2534StatusNoerror);
    EXPECT_EQ(Sent(), QByteArray("ats3 1 10400\r\nats3 3 0\r\n"));
}

TEST_F(J2534UnixTest, fastInit_sendsHeaderThenPayload)
{
    PassThruMsg request{};
    request.data_size = 2;
    request.data[0] = 0x81;
    request.data[1] = 0x10;

    ASSERT_EQ(j2534_.PassThruIoctl(3, kJ2534FastInit, &request, nullptr), kJ2534StatusNoerror);
    EXPECT_EQ(Sent(), QByteArray("aty3 2 0\r\n\x81\x10"));
}

const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment());

} // namespace
