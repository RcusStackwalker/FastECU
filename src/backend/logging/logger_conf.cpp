#include "src/backend/logging/logger_conf.h"

#include <format>
#include <sstream>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{
namespace
{

// QDomDocument::save(output, 4) wrote four spaces per level. pugixml defaults
// to a tab; matching the old indent spares every existing user a one-time
// whole-file reflow on the first write.
constexpr const char *kIndent = "    ";

constexpr std::size_t kGaugeCap = 15;
constexpr std::size_t kLowerPanelCap = 12;
constexpr std::size_t kSwitchCap = 20;

Status load(pugi::xml_document& document, bytes::ByteView conf, std::string_view source)
{
    // parse_default excludes comments, processing instructions and DOCTYPE.
    // A user who hand-annotated their conf file with an XML comment would
    // otherwise lose it silently on the first write_selection() round-trip
    // (load, then re-serialize the whole DOM) -- add them back in.
    const pugi::xml_parse_result parsed = document.load_buffer(
        conf.data(),
        conf.size(),
        pugi::parse_default | pugi::parse_comments | pugi::parse_pi | pugi::parse_doctype);
    if (!parsed)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }
    return {};
}

pugi::xml_node find_ecu(pugi::xml_node logger, std::string_view ecu_id)
{
    for (pugi::xml_node ecu : logger.children("ecu"))
    {
        if (ecu.attribute("id").value() == ecu_id)
        {
            return ecu;
        }
    }
    return {};
}

void append_ids(
    pugi::xml_node parent,
    const char *element_name,
    const std::vector<std::string>& ids)
{
    for (const std::string& id : ids)
    {
        pugi::xml_node node = parent.append_child(element_name);
        node.append_attribute("id") = id.c_str();
        node.append_attribute("name") = "";
    }
}

std::vector<std::string> collect_ids(pugi::xml_node parent, const char *element_name)
{
    std::vector<std::string> ids;
    for (pugi::xml_node node : parent.children(element_name))
    {
        ids.emplace_back(node.attribute("id").as_string("No id"));
    }
    return ids;
}

LoggerSelection walk(const LoggerDefinition& definition, bool enabled_only)
{
    LoggerSelection selection;
    if (!definition.parameters.empty())
    {
        selection.protocol = definition.parameters.front().protocol;
    }
    for (const LoggerParameter& parameter : definition.parameters)
    {
        if (enabled_only && !parameter.enabled)
        {
            continue;
        }
        if (selection.gauge_ids.size() < kGaugeCap)
        {
            selection.gauge_ids.push_back(parameter.id);
        }
        if (selection.lower_panel_ids.size() < kLowerPanelCap)
        {
            selection.lower_panel_ids.push_back(parameter.id);
        }
    }
    for (const LoggerSwitch& paramswitch : definition.switches)
    {
        // The enabled-only walk gates on LoggerSwitch::enabled, mirroring
        // read_logger_conf's `log_switch_enabled.at(i) == "1"` check
        // (file_actions.cpp:990). The first-N walk (initial_selection)
        // deliberately does not filter, same as the gauge/lower-panel loop
        // above.
        if (enabled_only && !paramswitch.enabled)
        {
            continue;
        }
        if (selection.switch_ids.size() < kSwitchCap)
        {
            selection.switch_ids.push_back(paramswitch.id);
        }
    }
    return selection;
}

} // namespace

Result<std::optional<LoggerSelection>> read_selection(
    bytes::ByteView conf, std::string_view ecu_id, std::string_view source)
{
    pugi::xml_document document;
    if (auto loaded = load(document, conf, source); !loaded)
    {
        return std::unexpected(loaded.error());
    }

    const pugi::xml_node ecu = find_ecu(document.child("config").child("logger"), ecu_id);
    if (!ecu)
    {
        return std::optional<LoggerSelection>{};
    }

    LoggerSelection selection;
    const pugi::xml_node protocol = ecu.child("protocol");
    selection.protocol = protocol.attribute("id").as_string("No id");
    const pugi::xml_node parameters = protocol.child("parameters");
    selection.gauge_ids = collect_ids(parameters.child("gauges"), "parameter");
    selection.lower_panel_ids = collect_ids(parameters.child("lower_panel"), "parameter");
    selection.switch_ids = collect_ids(protocol.child("switches"), "switch");
    return std::optional<LoggerSelection>{std::move(selection)};
}

Result<bytes::Bytes> write_selection(
    bytes::ByteView conf,
    std::string_view ecu_id,
    const LoggerSelection& selection,
    std::string_view source)
{
    pugi::xml_document document;
    if (auto loaded = load(document, conf, source); !loaded)
    {
        return std::unexpected(loaded.error());
    }

    pugi::xml_node config = document.child("config");
    if (!config)
    {
        config = document.append_child("config");
    }
    pugi::xml_node logger = config.child("logger");
    if (!logger)
    {
        logger = config.append_child("logger");
    }

    // Rebuild rather than patch attributes by index. The legacy writer walked
    // existing elements and set their `id` positionally, which silently
    // dropped ids when the selection was longer than the stored subtree.
    pugi::xml_node ecu = find_ecu(logger, ecu_id);
    if (ecu)
    {
        logger.remove_child(ecu);
    }
    ecu = logger.append_child("ecu");
    ecu.append_attribute("id") = std::string(ecu_id).c_str();

    pugi::xml_node protocol = ecu.append_child("protocol");
    protocol.append_attribute("id") = selection.protocol.c_str();
    pugi::xml_node parameters = protocol.append_child("parameters");
    append_ids(parameters.append_child("gauges"), "parameter", selection.gauge_ids);
    append_ids(parameters.append_child("lower_panel"), "parameter", selection.lower_panel_ids);
    append_ids(protocol.append_child("switches"), "switch", selection.switch_ids);

    std::ostringstream output;
    // format_no_declaration: the legacy QDomDocument::save(output, 4) never
    // wrote an <?xml ...?> prologue for these documents (Task 1's captured
    // golden has none), and pugixml's default *does* synthesize one when the
    // document lacks a declaration node. Passing format_default here would
    // prepend a declaration the golden -- and every existing conf file --
    // does not have.
    document.save(
        output,
        kIndent,
        pugi::format_indent | pugi::format_no_declaration,
        pugi::encoding_utf8);
    std::string xml = std::move(output).str();

    // pugixml's node_output_start unconditionally writes a space before a
    // self-closing tag's `/>` unless format_raw is set, and format_raw also
    // zeroes indent_length -- so the space can't be dropped via any save()
    // flag combination without losing the four-space indent above. Since
    // document.save() re-serializes the *entire* DOM (pugixml does not
    // preserve a loaded node's original formatting), every self-closing
    // element in the file gets this extra space, not just the <ecu> subtree
    // this function rebuilds -- which would reflow every leaf line of an
    // existing conf on first write, exactly what the four-space indent above
    // exists to avoid. Strip it back out post-serialization instead.
    //
    // The exact 4-byte needle " />\n" is safe to replace unconditionally:
    // under format_indent every empty-element tag is newline-terminated, so
    // a real tag end always looks like `.../>` followed immediately by '\n'.
    // pugixml does not escape '>' inside attribute values (its
    // chartypex_table marks '>' unescaped there), so a literal `name="x />
    // y"` value could contain the 3-byte " />" -- but never followed
    // directly by an unescaped '\n', because pugixml *does* escape '\n'
    // inside attribute values (emitted as "&#10;"). Outside attributes, in
    // PCDATA, '>' is escaped to "&gt;", so " />" can't occur unescaped there
    // either. So " />\n" can only ever be a tag terminator, never attribute
    // or PCDATA data.
    //
    // Caveat: load()'s parse_comments | parse_pi | parse_doctype flags (see
    // above) mean comment and PI text round-trips verbatim -- pugixml does
    // not apply the attribute/PCDATA escaping rules to it. A user's comment
    // whose text happens to contain the literal sequence " />\n" would have
    // that one space stripped from the comment body. That is a cosmetic
    // edit to comment text, not data loss, and is strictly better than the
    // alternative of dropping the comment outright, which is what the
    // pre-fix parse flags did.
    constexpr std::string_view kSelfCloseWithSpace = " />\n";
    constexpr std::string_view kSelfCloseNoSpace = "/>\n";
    for (std::size_t pos = xml.find(kSelfCloseWithSpace); pos != std::string::npos;
         pos = xml.find(kSelfCloseWithSpace, pos + kSelfCloseNoSpace.size()))
    {
        xml.replace(pos, kSelfCloseWithSpace.size(), kSelfCloseNoSpace);
    }

    return bytes::Bytes(xml.begin(), xml.end());
}

LoggerSelection initial_selection(const LoggerDefinition& definition)
{
    return walk(definition, /*enabled_only=*/false);
}

LoggerSelection default_selection(const LoggerDefinition& definition)
{
    return walk(definition, /*enabled_only=*/true);
}

} // namespace fastecu::logging
