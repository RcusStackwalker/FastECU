#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"

#include <QString>

#include <exception>
#include <functional>
#include <initializer_list>
#include <string>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

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
Status run_setters(std::initializer_list<Setter> setters)
{
    for (const Setter& setter : setters)
    {
        if (!setter.call())
        {
            return fail(ErrorKind::InvalidConfig, std::string(setter.name) + " failed");
        }
    }
    return {};
}

template <class F> auto guarded(F&& body) -> decltype(body())
{
    try
    {
        return body();
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "diagnostic link driver exception");
    }
}

Status no_facade()
{
    return fail(ErrorKind::Disconnected, "no serial facade");
}

} // namespace

Status SerialDiagnosticLink::open(const KlineLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            if (auto applied = run_setters({
                    {"set_is_iso14230_connection",
                     [&] { return serial_->set_is_iso14230_connection(c.iso14230_connection); }},
                    {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(c.header == KlineHeader::Ssm); }},
                    {"set_add_iso9141_header",
                     [&] { return serial_->set_add_iso9141_header(c.header == KlineHeader::Iso9141); }},
                    {"set_add_iso14230_header",
                     [&] { return serial_->set_add_iso14230_header(c.header == KlineHeader::Iso14230); }},
                    {"set_serial_port_baudrate",
                     [&] { return serial_->set_serial_port_baudrate(QString::number(c.baud)); }},
                    {"set_kline_startbyte", [&] { return serial_->set_kline_startbyte(c.start_byte); }},
                    {"set_kline_tester_id", [&] { return serial_->set_kline_tester_id(c.tester_id); }},
                    {"set_kline_target_id", [&] { return serial_->set_kline_target_id(c.target_id); }},
                    {"set_is_can_connection", [&] { return serial_->set_is_can_connection(false); }},
                    {"set_is_iso15765_connection", [&] { return serial_->set_is_iso15765_connection(false); }},
                    {"set_is_29_bit_id", [&] { return serial_->set_is_29_bit_id(false); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->open_serial_port().isEmpty())
            {
                return fail(ErrorKind::Disconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::open(const CanLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            if (auto applied = run_setters({
                    {"set_is_iso14230_connection", [&] { return serial_->set_is_iso14230_connection(false); }},
                    {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(false); }},
                    {"set_add_iso9141_header", [&] { return serial_->set_add_iso9141_header(false); }},
                    {"set_add_iso14230_header", [&] { return serial_->set_add_iso14230_header(false); }},
                    {"set_is_can_connection", [&] { return serial_->set_is_can_connection(!c.iso15765); }},
                    {"set_is_iso15765_connection", [&] { return serial_->set_is_iso15765_connection(c.iso15765); }},
                    {"set_is_29_bit_id", [&] { return serial_->set_is_29_bit_id(c.extended_id); }},
                    {"set_can_speed", [&] { return serial_->set_can_speed(QString::number(c.bitrate)); }},
                    {"set_iso15765_source_address", [&] { return serial_->set_iso15765_source_address(c.source_id); }},
                    {"set_iso15765_destination_address",
                     [&] { return serial_->set_iso15765_destination_address(c.destination_id); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->open_serial_port().isEmpty())
            {
                return fail(ErrorKind::Disconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::reset()
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            return {};
        });
}

Status SerialDiagnosticLink::set_header(KlineHeader header)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            return run_setters({
                {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(header == KlineHeader::Ssm); }},
                {"set_add_iso9141_header",
                 [&] { return serial_->set_add_iso9141_header(header == KlineHeader::Iso9141); }},
                {"set_add_iso14230_header",
                 [&] { return serial_->set_add_iso14230_header(header == KlineHeader::Iso14230); }},
            });
        });
}

Status SerialDiagnosticLink::set_p1_max(std::chrono::milliseconds p1_max)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            const int value = saturating_ms<int>(p1_max);
            if (serial_->get_use_openport2_adapter())
            {
                if (serial_->set_j2534_ioctl(kJ2534IoctlP1Max, value) != STATUS_SUCCESS)
                {
                    return fail(ErrorKind::Disconnected, "set_j2534_ioctl(P1_MAX) failed");
                }
                return {};
            }
            if (!serial_->set_kline_timings(SERIAL_P1_MAX, value))
            {
                return fail(ErrorKind::InvalidConfig, "set_kline_timings(P1_MAX) failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::five_baud_init(std::uint8_t address)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded(
        [&]() -> Result<bytes::Bytes>
        { return bytes::fromQByteArray(serial_->five_baud_init(QByteArray(1, static_cast<char>(address)))); });
}

Status SerialDiagnosticLink::fast_init(bytes::ByteView wakeup)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            if (serial_->fast_init(bytes::toQByteArray(wakeup)) != STATUS_SUCCESS)
            {
                return fail(ErrorKind::Disconnected, "fast_init failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::write(bytes::ByteView data)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded([&]() -> Result<bytes::Bytes>
                   { return bytes::fromQByteArray(serial_->write_serial_data_echo_check(bytes::toQByteArray(data))); });
}

namespace
{
template <class ReadCall>
Result<IDiagnosticLink::OptionalBytes> guarded_read(SerialPortActions *serial, const ICancellationToken& cancellation,
                                                    ReadCall read_call)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "diagnostic read cancelled before driver call");
    }
    if (serial == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded(
        [&]() -> Result<IDiagnosticLink::OptionalBytes>
        {
            const QByteArray raw = read_call();
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "diagnostic read cancelled");
            }
            if (raw.isEmpty())
            {
                return IDiagnosticLink::OptionalBytes{};
            }
            return IDiagnosticLink::OptionalBytes{bytes::fromQByteArray(raw)};
        });
}
} // namespace

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::read(std::chrono::milliseconds timeout,
                                                                  const ICancellationToken& cancellation)
{
    return guarded_read(serial_, cancellation,
                        [&] { return serial_->read_serial_data(saturating_ms<quint16>(timeout)); });
}

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::read_obd(std::chrono::milliseconds timeout,
                                                                      const ICancellationToken& cancellation)
{
    return guarded_read(serial_, cancellation,
                        [&] { return serial_->read_serial_obd_data(saturating_ms<quint16>(timeout)); });
}

bool SerialDiagnosticLink::uses_j2534() const
{
    try
    {
        return serial_ != nullptr && serial_->get_use_openport2_adapter();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace fastecu::diagnostics
