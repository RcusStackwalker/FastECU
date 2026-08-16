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

// QDomDocument::save(output, 4) wrote four spaces per level. pugixml defaults
// to a tab; matching the old indent spares every existing user a one-time
// whole-file reflow on the first write.
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
    const pugi::xml_parse_result parsed = document.load_buffer(
        conf.data(), conf.size(),
        pugi::parse_default | pugi::parse_comments | pugi::parse_pi | pugi::parse_doctype | pugi::parse_declaration);
    if (!parsed)
    {
        return fail(ErrorKind::InvalidConfig,
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
        // non-well-formed XML that QDom, and every conformant parser, refuses
        // to re-read even though pugixml is lenient enough to load it back.
        // The legacy writer left such a file untouched; refuse rather than
        // corrupt it.
        return fail(ErrorKind::InvalidConfig, std::format("{}: root element is <{}>, expected <config>", source,
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
    // The XML-declaration fidelity contract, established empirically against
    // QDomDocument (setContent + save(ts, 4)): an existing <?xml ...?>
    // declaration is PRESERVED, and one is NEVER synthesized when the input
    // has none. Two things implement exactly that here, and both are
    // required:
    //   - load()'s pugi::parse_declaration keeps the declaration as a real
    //     node_declaration in the DOM. pugixml's node_output writes such a
    //     node like any other, independent of the save flags.
    //   - format_no_declaration suppresses only pugixml's *synthesized*
    //     prologue -- xml_document::save() emits that solely under
    //     `!(flags & format_no_declaration) && !has_declaration(_root)`, so
    //     the flag cannot swallow a parsed declaration node.
    // Dropping either half regresses a real file: resources/shared/config/
    // logger.cfg opens with a declaration and provisioning.cpp copies it into
    // every user's config dir, so without parse_declaration the first save
    // silently deletes it; without format_no_declaration a conf that never had
    // one grows a prologue QDom would not have written.
    // pugixml re-emits the declaration's attribute values double-quoted where
    // QDom wrote them single-quoted; that requoting is cosmetic and the only
    // byte that changes when the real shipped logger.cfg round-trips here.
    // (An earlier revision of this comment inferred from Task 1's captured
    // golden that QDom "never wrote a prologue for these documents". That was
    // false: the golden has none because its *input* had none.)
    document.save(output, kIndent, pugi::format_indent | pugi::format_no_declaration, pugi::encoding_utf8);
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
