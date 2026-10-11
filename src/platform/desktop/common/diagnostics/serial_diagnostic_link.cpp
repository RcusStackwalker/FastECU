#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"

#include <QSerialPort>
#include <QString>

#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <string>

#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/backend/protocol/j2534/j2534_constants.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::diagnostics
{
namespace
{

struct Setter
{
    const char *name;
    std::function<bool()> call;
};

// Setters only record state (serial_port_actions_direct.h), so a false return
// is a configuration problem, never a lost adapter. Stops at the first
// failure so later setters are not reached.
Status RunSetters(std::initializer_list<Setter> setters)
{
    for (const Setter& setter : setters)
    {
        if (!setter.call())
        {
            return Fail(ErrorKind::kInvalidConfig, std::string(setter.name) + " failed");
        }
    }
    return {};
}

template <class F> auto Guarded(F&& body) -> decltype(body())
{
    try
    {
        return body();
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "diagnostic link driver exception");
    }
}

Status NoFacade()
{
    return Fail(ErrorKind::kDisconnected, "no serial facade");
}

} // namespace

Status SerialDiagnosticLink::Open(const KlineLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            serial_->ResetConnection();
            if (auto applied = RunSetters({
                    {"set_is_iso14230_connection",
                     [&] { return serial_->SetIsIso14230Connection(c.iso14230_connection); }},
                    {"set_add_ssm_header", [&] { return serial_->SetAddSsmHeader(c.header == KlineHeader::kSsm); }},
                    {"set_add_iso9141_header",
                     [&] { return serial_->SetAddIso9141Header(c.header == KlineHeader::kIso9141); }},
                    {"set_add_iso14230_header",
                     [&] { return serial_->SetAddIso14230Header(c.header == KlineHeader::kIso14230); }},
                    {"set_serial_port_baudrate",
                     [&] { return serial_->SetSerialPortBaudrate(QString::number(c.baud)); }},
                    {"set_serial_port_parity",
                     [&]
                     {
                         return serial_->SetSerialPortParity(static_cast<std::uint8_t>(
                             c.parity == Parity::kEven ? QSerialPort::EvenParity : QSerialPort::NoParity));
                     }},
                    {"set_kline_startbyte", [&] { return serial_->SetKlineStartbyte(c.start_byte); }},
                    {"set_kline_tester_id", [&] { return serial_->SetKlineTesterId(c.tester_id); }},
                    {"set_kline_target_id", [&] { return serial_->SetKlineTargetId(c.target_id); }},
                    {"set_is_can_connection", [&] { return serial_->SetIsCanConnection(false); }},
                    {"set_is_iso15765_connection", [&] { return serial_->SetIsIso15765Connection(false); }},
                    {"set_is_29_bit_id", [&] { return serial_->SetIs29BitId(false); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->OpenSerialPort().isEmpty())
            {
                return Fail(ErrorKind::kDisconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::Open(const CanLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            serial_->ResetConnection();
            if (auto applied = RunSetters({
                    {"set_is_iso14230_connection", [&] { return serial_->SetIsIso14230Connection(false); }},
                    {"set_add_ssm_header", [&] { return serial_->SetAddSsmHeader(false); }},
                    {"set_add_iso9141_header", [&] { return serial_->SetAddIso9141Header(false); }},
                    {"set_add_iso14230_header", [&] { return serial_->SetAddIso14230Header(false); }},
                    {"set_is_can_connection", [&] { return serial_->SetIsCanConnection(!c.iso15765); }},
                    {"set_is_iso15765_connection", [&] { return serial_->SetIsIso15765Connection(c.iso15765); }},
                    {"set_is_29_bit_id", [&] { return serial_->SetIs29BitId(c.extended_id); }},
                    {"set_can_speed", [&] { return serial_->SetCanSpeed(QString::number(c.bitrate)); }},
                    {"set_iso15765_source_address", [&] { return serial_->SetIso15765SourceAddress(c.source_id); }},
                    {"set_iso15765_destination_address",
                     [&] { return serial_->SetIso15765DestinationAddress(c.destination_id); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->OpenSerialPort().isEmpty())
            {
                return Fail(ErrorKind::kDisconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::Reset()
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            serial_->ResetConnection();
            return {};
        });
}

Status SerialDiagnosticLink::SetHeader(KlineHeader header)
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            return RunSetters({
                {"set_add_ssm_header", [&] { return serial_->SetAddSsmHeader(header == KlineHeader::kSsm); }},
                {"set_add_iso9141_header",
                 [&] { return serial_->SetAddIso9141Header(header == KlineHeader::kIso9141); }},
                {"set_add_iso14230_header",
                 [&] { return serial_->SetAddIso14230Header(header == KlineHeader::kIso14230); }},
            });
        });
}

Status SerialDiagnosticLink::SetP1Max(std::chrono::milliseconds p1_max)
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            const int value = SaturatingMs<int>(p1_max);
            if (serial_->GetUseOpenport2Adapter())
            {
                if (serial_->SetJ2534Ioctl(kJ2534P1Max, value) != kSerialSuccess)
                {
                    return Fail(ErrorKind::kDisconnected, "set_j2534_ioctl(P1_MAX) failed");
                }
                return {};
            }
            if (!serial_->SetKlineTimings(kSerialP1Max, value))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_kline_timings(P1_MAX) failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::FiveBaudInit(std::uint8_t address)
{
    if (serial_ == nullptr)
    {
        return Fail(ErrorKind::kDisconnected, "no serial facade");
    }
    return Guarded([&]() -> Result<bytes::Bytes>
                   { return bytes::FromQByteArray(serial_->FiveBaudInit(QByteArray(1, static_cast<char>(address)))); });
}

Status SerialDiagnosticLink::FastInit(bytes::ByteView wakeup)
{
    if (serial_ == nullptr)
    {
        return NoFacade();
    }
    return Guarded(
        [&]() -> Status
        {
            if (serial_->FastInit(bytes::ToQByteArray(wakeup)) != kSerialSuccess)
            {
                return Fail(ErrorKind::kDisconnected, "fast_init failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::Write(bytes::ByteView data)
{
    if (serial_ == nullptr)
    {
        return Fail(ErrorKind::kDisconnected, "no serial facade");
    }
    return Guarded([&]() -> Result<bytes::Bytes>
                   { return bytes::FromQByteArray(serial_->WriteSerialDataEchoCheck(bytes::ToQByteArray(data))); });
}

namespace
{
template <class ReadCall>
Result<IDiagnosticLink::OptionalBytes> GuardedRead(SerialPortActions *serial, const ICancellationToken& cancellation,
                                                   ReadCall read_call)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "diagnostic read cancelled before driver call");
    }
    if (serial == nullptr)
    {
        return Fail(ErrorKind::kDisconnected, "no serial facade");
    }
    return Guarded(
        [&]() -> Result<IDiagnosticLink::OptionalBytes>
        {
            const QByteArray raw = read_call();
            if (cancellation.Cancelled())
            {
                return Fail(ErrorKind::kCancelled, "diagnostic read cancelled");
            }
            if (raw.isEmpty())
            {
                return IDiagnosticLink::OptionalBytes{};
            }
            return IDiagnosticLink::OptionalBytes{bytes::FromQByteArray(raw)};
        });
}
} // namespace

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::Read(std::chrono::milliseconds timeout,
                                                                  const ICancellationToken& cancellation)
{
    return GuardedRead(serial_, cancellation, [&] { return serial_->ReadSerialData(SaturatingMs<quint16>(timeout)); });
}

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::ReadObd(std::chrono::milliseconds timeout,
                                                                     const ICancellationToken& cancellation)
{
    return GuardedRead(serial_, cancellation,
                       [&] { return serial_->ReadSerialObdData(SaturatingMs<quint16>(timeout)); });
}

bool SerialDiagnosticLink::UsesJ2534() const
{
    try
    {
        return serial_ != nullptr && serial_->GetUseOpenport2Adapter();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace fastecu::diagnostics
