#include "src/backend/logging/logger_conf.h"

#include <format>
#include <ranges>
#include <sstream>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{

using namespace std::literals::string_view_literals;

namespace
{

// Four spaces per level; pugixml defaults to a tab. Existing user confs are
// four-space indented, so this keeps a save from reflowing the whole file.
constexpr const char *kIndent = "    ";

constexpr std::size_t kGaugeCap = 15;
constexpr std::size_t kLowerPanelCap = 12;
constexpr std::size_t kSwitchCap = 20;

Status load(pugi::xml_document& document, bytes::ByteView conf, std::string_view source)
{
    // parse_default excludes comments, processing instructions, DOCTYPE and
    // the XML declaration. A user who hand-annotated their conf file with an
    // XML comment -- or, far more commonly, whose conf simply starts with the
    // <?xml ...?> declaration that resources/shared/config/logger.cfg ships
    // with -- would otherwise lose it silently on the first write_selection()
    // round-trip (load, then re-serialize the whole DOM) -- add them back in.
    if (const pugi::xml_parse_result parsed =
            document.load_buffer(conf.data(), conf.size(),
                                 pugi::parse_default | pugi::parse_comments | pugi::parse_pi | pugi::parse_doctype |
                                     pugi::parse_declaration);
        !parsed)
    {
        return fail(ErrorKind::kInvalidConfig,
                    std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }
    return {};
}

pugi::xml_node find_ecu(pugi::xml_node logger, std::string_view ecu_id)
{
    const auto ecus = logger.children("ecu");
    const auto it = std::ranges::find(ecus, ecu_id, [](pugi::xml_node n) { return n.attribute("id"sv).value(); });
    return it != ecus.end() ? *it : pugi::xml_node{};
}

void append_ids(pugi::xml_node parent, std::string_view element_name, const std::vector<std::string>& ids)
{
    for (const std::string& id : ids)
    {
        pugi::xml_node node = parent.append_child(element_name);
        node.append_attribute("id"sv) = id;
        node.append_attribute("name"sv) = ""sv;
    }
}

/* element_name has to be const char* because xml_node::children doesn't accept string_view*/
std::vector<std::string> collect_ids(pugi::xml_node parent, const char *element_name)
{
    return parent.children(element_name) |
           std::views::transform([](pugi::xml_node node)
                                 { return std::string{node.attribute("id"sv).as_string("No id")}; }) |
           std::ranges::to<std::vector>();
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

Result<std::optional<LoggerSelection>> read_selection(bytes::ByteView conf, std::string_view ecu_id,
                                                      std::string_view source)
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

Result<bytes::Bytes> write_selection(bytes::ByteView conf, std::string_view ecu_id, const LoggerSelection& selection,
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
        // Some other element is already the document element -- pugixml
        // rejects a buffer with no element at all (status_no_document_element,
        // parse_fragment is not set), so a successful load() always leaves
        // one. Appending <config> here would emit two document elements:
        // non-well-formed XML that every conformant parser refuses
        // to re-read even though pugixml is lenient enough to load it back.
        // Refuse rather than corrupt it.
        return fail(ErrorKind::kInvalidConfig, std::format("{}: root element is <{}>, expected <config>", source,
                                                           document.document_element().name()));
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
        // Reinsert the rebuilt element where the old one sat. document.save()
        // re-serializes the whole DOM, so append_child() would visibly
        // relocate an updated <ecu> below all of its siblings in the file --
        // byte-neutral, but a gratuitous diff in a file this function
        // otherwise works hard to leave alone.
        const pugi::xml_node previous = ecu.previous_sibling();
        logger.remove_child(ecu);
        ecu = previous ? logger.insert_child_after("ecu", previous) : logger.prepend_child("ecu");
    }
    else
    {
        ecu = logger.append_child("ecu");
    }
    ecu.append_attribute("id") = ecu_id;

    pugi::xml_node protocol = ecu.append_child("protocol");
    protocol.append_attribute("id") = selection.protocol;
    pugi::xml_node parameters = protocol.append_child("parameters");
    append_ids(parameters.append_child("gauges"), "parameter", selection.gauge_ids);
    append_ids(parameters.append_child("lower_panel"), "parameter", selection.lower_panel_ids);
    append_ids(protocol.append_child("switches"), "switch", selection.switch_ids);

    std::ostringstream output;
    // An existing <?xml ...?> declaration is preserved and one is never
    // synthesized when the input has none. Both halves are required:
    //   - load()'s pugi::parse_declaration keeps the declaration as a real
    //     node_declaration, which save() writes like any other node.
    //   - format_no_declaration suppresses only pugixml's synthesized prologue.
    // resources/shared/config/logger.cfg opens with a declaration and
    // provisioning.cpp copies it into every user's config dir, so dropping
    // either half would edit every user's file on first save. pugixml re-emits
    // the declaration's attribute values double-quoted; that requoting is cosmetic.
    document.save(output, kIndent, pugi::format_indent | pugi::format_no_declaration, pugi::encoding_utf8);
    const std::string xml = std::move(output).str();

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
