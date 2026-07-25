#include "src/backend/config/protocol_catalog.h"

#include <pugixml.hpp>

namespace fastecu::config
{
namespace
{

std::string text_or_empty(pugi::xml_node protocol, const char *tag)
{
    return protocol.child(tag).text().as_string();
}

} // namespace

Result<ProtocolCatalog> load_protocol_catalog(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<std::vector<std::uint8_t>> bytes = file_repository.read(paths.protocols_file);
    if (!bytes.has_value())
        return std::unexpected(bytes.error());

    pugi::xml_document doc;
    pugi::xml_parse_result parsed = doc.load_buffer(bytes->data(), bytes->size());
    if (!parsed)
        return fail(ErrorKind::InvalidConfig, std::string("protocols parse error: ") + parsed.description());

    ProtocolCatalog catalog;
    pugi::xml_node protocols = doc.child("config").child("protocols");
    for (pugi::xml_node protocol : protocols.children("protocol"))
    {
        ProtocolEntry entry;
        // Legacy read_protocols_file (file_actions.cpp:1146/1148) reads
        // these via Qt's three-arg QDomElement::attribute(name, default),
        // defaulting to the literal strings "No name"/"No alias" when the
        // attribute is absent -- not empty string. 42 of the 61 real
        // shipped <protocol> elements omit alias, so this default is hit in
        // the majority of real entries; match it exactly.
        entry.protocol_name = protocol.attribute("name").as_string("No name");
        entry.alias = protocol.attribute("alias").as_string("No alias");
        entry.ecu = text_or_empty(protocol, "ecu");
        entry.mcu = text_or_empty(protocol, "mcu");
        entry.mode = text_or_empty(protocol, "mode");
        entry.checksum = text_or_empty(protocol, "checksum");
        entry.read = text_or_empty(protocol, "read");
        entry.test_write = text_or_empty(protocol, "test_write");
        entry.write = text_or_empty(protocol, "write");
        entry.flash_transport = text_or_empty(protocol, "flash_transport");
        entry.log_transport = text_or_empty(protocol, "log_transport");
        entry.log_protocol = text_or_empty(protocol, "log_protocol");
        entry.ecu_id_ascii = text_or_empty(protocol, "ecu_id_ascii");
        entry.ecu_id_addr = text_or_empty(protocol, "ecu_id_addr");
        entry.ecu_id_length = text_or_empty(protocol, "ecu_id_length");
        entry.cal_id_ascii = text_or_empty(protocol, "cal_id_ascii");
        entry.cal_id_addr = text_or_empty(protocol, "cal_id_addr");
        entry.cal_id_length = text_or_empty(protocol, "cal_id_length");
        entry.kernel = text_or_empty(protocol, "kernel");
        entry.kernel_addr = text_or_empty(protocol, "kernel_addr");
        entry.description = text_or_empty(protocol, "description");
        catalog.push_back(std::move(entry));
    }

    return catalog;
}

} // namespace fastecu::config
