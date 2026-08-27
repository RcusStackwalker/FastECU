#include "src/platform/desktop/common/connection/j2534_adapter_provider.h"

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"

#include <QByteArray>
#include <QtGlobal>

#include <cstdint>
#include <exception>
#include <format>
#include <utility>

namespace fastecu::desktop::connection
{
namespace
{

constexpr std::string_view kCandidatePrefix = "j2534:";

class OpenedJ2534Adapter final : public OpenedCanAdapter
{
  public:
    explicit OpenedJ2534Adapter(std::unique_ptr<SerialPortActions> serial) : serial_(std::move(serial))
    {
    }

    std::unique_ptr<cdbg::ICanTransport> into_transport() && override
    {
        return std::make_unique<cdbg::FastEcuCanTransport>(std::move(serial_));
    }

  private:
    std::unique_ptr<SerialPortActions> serial_;
};

std::string candidate_id_for(const QString& entry)
{
    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    std::uint64_t hash = kFnvOffsetBasis;
    const QByteArray bytes = entry.toUtf8();
    for (const char byte : bytes)
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kFnvPrime;
    }
    return std::format("{}{:016x}", kCandidatePrefix, hash);
}

bool starts_with_port_name(QStringView entry)
{
    return entry.startsWith(QStringView(u"tty"), Qt::CaseInsensitive) ||
           entry.startsWith(QStringView(u"cu."), Qt::CaseInsensitive) ||
           entry.startsWith(QStringView(u"/dev/"), Qt::CaseInsensitive) ||
           (entry.size() > 3 && entry.startsWith(QStringView(u"COM"), Qt::CaseInsensitive) && entry[3].isDigit());
}

bool is_remote_entry(QStringView entry)
{
    return entry.contains(QStringView(u"://"), Qt::CaseInsensitive);
}

bool is_local_j2534_entry(const QString& entry)
{
    const QStringView view(entry);
    const bool names_openport = view.contains(QStringView(u"OpenPort 2.0"), Qt::CaseInsensitive);
    const bool names_j2534 = view.contains(QStringView(u"J2534"), Qt::CaseInsensitive);

    if (entry.trimmed().isEmpty() || is_remote_entry(view))
    {
        return false;
    }
    if (starts_with_port_name(view) && !names_openport)
    {
        return false;
    }

    // isJ2534CapableEntry() is the direct backend's authoritative OpenPort
    // branch predicate. Windows check_serial_ports() also returns ordinary
    // serial entries, so a non-OpenPort candidate must explicitly identify
    // itself as J2534 rather than relying on Windows' broad backend predicate.
    return names_j2534 || (names_openport && isJ2534CapableEntry(view));
}

QString remove_suffix(QString text, QStringView suffix)
{
    if (text.endsWith(suffix, Qt::CaseInsensitive))
    {
        text.chop(suffix.size());
    }
    return text.trimmed();
}

LocalAdapterDescriptor descriptor_for(const std::string& candidate_id, const QString& raw_entry)
{
    const QString entry = raw_entry.trimmed();
    if (entry.contains(QStringView(u"OpenPort 2.0"), Qt::CaseInsensitive))
    {
        return {
            .candidate_id = candidate_id,
            .kind = dashboard::AdapterKind::J2534,
            .vendor = "Tactrix",
            .display_name = "OpenPort 2.0",
            .label = "Tactrix OpenPort 2.0",
        };
    }

    const qsizetype separator = entry.indexOf(QStringView(u" - "));
    QString vendor;
    QString display_name;
    if (separator >= 0)
    {
        vendor = entry.first(separator).trimmed();
        display_name = entry.sliced(separator + 3).trimmed();
        display_name = remove_suffix(std::move(display_name), QStringView(u" J2534 DLL"));
        display_name = remove_suffix(std::move(display_name), QStringView(u" J2534"));
    }
    else
    {
        vendor = remove_suffix(entry, QStringView(u" J2534 DLL"));
        vendor = remove_suffix(std::move(vendor), QStringView(u" J2534"));
        display_name = QStringLiteral("J2534");
    }

    if (vendor.isEmpty())
    {
        vendor = QStringLiteral("J2534");
    }
    if (display_name.isEmpty())
    {
        display_name = QStringLiteral("J2534");
    }

    return {
        .candidate_id = candidate_id,
        .kind = dashboard::AdapterKind::J2534,
        .vendor = vendor.toStdString(),
        .display_name = display_name.toStdString(),
        .label = QStringLiteral("%1 %2").arg(vendor, display_name).toStdString(),
    };
}

logging::RawCanSetupActions raw_can_actions(SerialPortActions& serial)
{
    return {
        .set_iso14230 = [&serial](bool enabled) { return serial.set_is_iso14230_connection(enabled); },
        .set_iso14230_header = [&serial](bool enabled) { return serial.set_add_iso14230_header(enabled); },
        .set_raw_can = [&serial](bool enabled) { return serial.set_is_can_connection(enabled); },
        .set_iso15765 = [&serial](bool enabled) { return serial.set_is_iso15765_connection(enabled); },
        .set_identifier_width = [&serial](dashboard::CanIdentifierWidth width)
        { return serial.set_is_29_bit_id(width == dashboard::CanIdentifierWidth::Extended); },
        .set_bitrate = [&serial](std::uint32_t bitrate) { return serial.set_can_speed(QString::number(bitrate)); },
        .set_reply_id = [&serial](std::uint32_t id) { return serial.set_can_destination_address(id); },
    };
}

} // namespace

J2534AdapterProvider::J2534AdapterProvider()
    : J2534AdapterProvider(
          Dependencies{
              .list = [](SerialPortActions& serial) { return serial.check_serial_ports(); },
              .construct = [] { return std::make_unique<SerialPortActions>(QString{}); },
              .configure = [](SerialPortActions& serial, const logging::RawCanSetupProfile& profile)
              { return logging::configure_raw_can(profile, raw_can_actions(serial)); },
              .open = [](SerialPortActions& serial) { return serial.open_serial_port(); },
              .is_open = [](SerialPortActions& serial) { return serial.is_serial_port_open(); },
          },
          TestingTag{})
{
}

J2534AdapterProvider::J2534AdapterProvider(Dependencies dependencies, TestingTag)
    : dependencies_(std::move(dependencies))
{
}

dashboard::AdapterKind J2534AdapterProvider::kind() const
{
    return dashboard::AdapterKind::J2534;
}

Result<std::vector<LocalAdapterDescriptor>> J2534AdapterProvider::discover()
{
    try
    {
        if (!dependencies_.construct || !dependencies_.list)
        {
            return fail(ErrorKind::Internal, "J2534 discovery actions are unavailable");
        }
        auto serial = dependencies_.construct();
        if (!serial)
        {
            return fail(ErrorKind::Internal, "J2534 discovery failed to construct serial actions");
        }

        std::vector<LocalAdapterDescriptor> candidates;
        std::unordered_map<std::string, QString> issued_entries;
        for (const QString& entry : dependencies_.list(*serial))
        {
            if (!is_local_j2534_entry(entry))
            {
                continue;
            }

            const std::string candidate_id = candidate_id_for(entry);
            const auto [existing, inserted] = issued_entries.emplace(candidate_id, entry);
            if (!inserted)
            {
                if (existing->second != entry)
                {
                    return fail(ErrorKind::Internal, "J2534 candidate identifier collision");
                }
                continue;
            }
            candidates.push_back(descriptor_for(candidate_id, entry));
        }

        issued_entries_ = std::move(issued_entries);
        return candidates;
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "J2534 discovery threw an unknown exception");
    }
}

Result<std::unique_ptr<OpenedCanAdapter>> J2534AdapterProvider::open(std::string_view candidate_id,
                                                                     const dashboard::CdbgConnectionProfile& profile)
{
    const auto selected = issued_entries_.find(std::string(candidate_id));
    if (selected == issued_entries_.end())
    {
        return fail(ErrorKind::InvalidConfig, "J2534 candidate was not issued by this provider");
    }

    try
    {
        if (!dependencies_.construct || !dependencies_.configure || !dependencies_.open || !dependencies_.is_open)
        {
            return fail(ErrorKind::Internal, "J2534 open actions are unavailable");
        }
        auto serial = dependencies_.construct();
        if (!serial)
        {
            return fail(ErrorKind::Internal, "J2534 open failed to construct serial actions");
        }
        if (!serial->set_serial_port_list(QStringList{selected->second}))
        {
            return fail(ErrorKind::InvalidConfig, "failed to select J2534 adapter");
        }

        const logging::RawCanSetupProfile raw_profile{
            .bitrate = profile.bitrate,
            .identifier_width = profile.identifier_width,
            .reply_id = profile.reply_id,
        };
        if (const Status configured = dependencies_.configure(*serial, raw_profile); !configured.has_value())
        {
            return std::unexpected(configured.error());
        }
        if (dependencies_.open(*serial).isEmpty() || !dependencies_.is_open(*serial))
        {
            return fail(ErrorKind::Disconnected, "unable to open local J2534 adapter");
        }

        std::unique_ptr<OpenedCanAdapter> opened = std::make_unique<OpenedJ2534Adapter>(std::move(serial));
        return opened;
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "J2534 open threw an unknown exception");
    }
}

} // namespace fastecu::desktop::connection
