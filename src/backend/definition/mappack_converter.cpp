#include "src/backend/definition/mappack_converter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>
#include <pugixml.hpp>

namespace fastecu::definition
{
namespace
{
using Row = std::vector<std::string>;

bool contains_xml_control(std::string_view text)
{
    return std::ranges::any_of(text,
                               [](unsigned char ch) { return ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r'; });
}

std::unexpected<Error> invalid(std::size_t row, std::string_view column, std::string_view reason)
{
    return fail(ErrorKind::InvalidConfig, std::format("CSV row {}, column {}: {}", row, column, reason));
}

// Consume one field, leaving the record separator for the caller.
Result<std::string> parse_field(std::string_view& csv, std::size_t row, std::size_t column)
{
    const auto error = [&](std::string_view reason) { return invalid(row, std::to_string(column), reason); };
    if (!csv.starts_with('"'))
    {
        const auto end = csv.find_first_of(";\r\n");
        const auto field = csv.substr(0, end);
        if (field.contains('"'))
        {
            return error("unexpected quote in unquoted field");
        }
        csv.remove_prefix(field.size());
        return std::string(field);
    }
    csv.remove_prefix(1);
    std::string field;
    while (!csv.empty())
    {
        const auto quote = csv.find('"');
        if (quote == std::string_view::npos)
        {
            break;
        }
        field += csv.substr(0, quote);
        csv.remove_prefix(quote + 1);
        if (csv.starts_with('"'))
        {
            field += '"';
            csv.remove_prefix(1);
            continue;
        }
        if (!csv.empty() && csv.front() != ';' && csv.front() != '\r' && csv.front() != '\n')
        {
            return error("unexpected character after quoted field");
        }
        return field;
    }
    return error("unterminated quoted field");
}

Result<std::vector<Row>> parse_csv(std::string_view csv)
{
    std::vector<Row> rows;
    Row row;
    while (!csv.empty())
    {
        auto field = parse_field(csv, rows.size() + 1, row.size() + 1);
        if (!field.has_value())
        {
            return std::unexpected(field.error());
        }
        if (contains_xml_control(*field))
        {
            return invalid(rows.size() + 1, std::to_string(row.size() + 1), "XML-invalid control character");
        }
        row.push_back(std::move(*field));
        if (csv.empty())
        {
            break;
        }
        const char separator = csv.front();
        csv.remove_prefix(1);
        if (separator == ';')
        {
            if (csv.empty())
            {
                row.emplace_back();
            }
            continue;
        }
        if (separator == '\r' && csv.starts_with('\n'))
        {
            csv.remove_prefix(1);
        }
        rows.push_back(std::move(row));
        row.clear();
    }
    if (!row.empty())
    {
        rows.push_back(std::move(row));
    }
    return rows;
}

template <typename T> bool parse_number(std::string_view text, T& value, int base = 10)
{
    if (text.empty())
    {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string decimal(std::string value)
{
    std::ranges::replace(value, ',', '.');
    return value;
}

void attribute(pugi::xml_node node, const char *name, std::string_view value)
{
    node.append_attribute(name).set_value(value.data(), value.size());
}

void storage(pugi::xml_node node, std::string_view org)
{
    const char *type = "uint8";
    if (org == "eHiLo")
    {
        type = "uint16";
    }
    else if (org == "eHiLoHiLo")
    {
        type = "uint32";
    }
    attribute(node, "storagetype", type);
    attribute(node, "endian", "big");
}

void scaling(pugi::xml_node table, std::string_view units, std::string factor, int precision)
{
    auto node = table.append_child("scaling");
    attribute(node, "units", units);
    factor = decimal(std::move(factor));
    attribute(node, "expression", "x*" + factor);
    attribute(node, "to_byte", "x/" + factor);
    attribute(node, "format", precision == 0 ? "#" : "#0." + std::string(precision, '0'));
    attribute(node, "fineincrement", "1");
    attribute(node, "coarseincrement", "1");
}

pugi::xml_node rom_id(pugi::xml_node rom, std::string_view id)
{
    auto node = rom.append_child("romid");
    node.append_child("xmlid").text().set(std::string(id).c_str());
    for (const auto *name : {"internalidaddress", "internalidstring", "ecuid", "year", "market", "make", "model",
                             "submodel", "transmission", "memmodel", "flashmethod", "filesize", "checksummodule"})
    {
        node.append_child(name);
    }
    return node;
}
using Columns = std::map<std::string, std::size_t, std::less<>>;

Result<Columns> parse_columns(Row& header)
{
    // MapPack exports may terminate each record with an extra semicolon.
    if (header.back().empty())
    {
        header.pop_back();
    }
    Columns columns;
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        if (!columns.try_emplace(header[i], i).second)
        {
            return invalid(1, header[i], "duplicate header");
        }
    }
    for (const auto *name : {"Name", "FolderName", "DataOrg", "Columns", "Rows", "Fieldvalues.Name", "Fieldvalues.Unit",
                             "Fieldvalues.Factor", "Precision", "Fieldvalues.StartAddr", "Comment"})
    {
        if (!columns.contains(name))
        {
            return invalid(1, name, "missing required header");
        }
    }
    return columns;
}

struct MapRow
{
    const Row& fields;
    const Columns& columns;
    std::size_t number;

    const std::string& get(std::string_view name) const
    {
        return fields.at(columns.find(name)->second);
    }
};

Result<int> scaling_precision(const MapRow& row, std::string_view factor_name, std::string_view precision_name)
{
    double factor = 0;
    const auto text = decimal(row.get(factor_name));
    if (const auto result = std::from_chars(text.data(), text.data() + text.size(), factor);
        result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(factor) || factor == 0)
    {
        return invalid(row.number, factor_name, "expected finite nonzero factor");
    }
    int precision = 0;
    if (!parse_number(row.get(precision_name), precision) || precision < 0 || precision > 100)
    {
        return invalid(row.number, precision_name, "expected precision between 0 and 100");
    }
    return precision;
}

Result<std::string> storage_address(const MapRow& row, std::string_view name)
{
    std::string_view text = row.get(name);
    if (text.starts_with('$'))
    {
        text.remove_prefix(1);
    }
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }
    if (std::uint64_t value = 0; !parse_number(text, value, 16))
    {
        return invalid(row.number, name, "expected hexadecimal address");
    }
    return "0x" + std::string(text);
}

Status append_axis(pugi::xml_node table, pugi::xml_node located, const MapRow& row, const std::string& prefix,
                   std::string_view type, int size)
{
    // DataHeader controls address emission independently of axis size in legacy exports.
    if (size <= 1 && !row.columns.contains(prefix + "DataHeader"))
    {
        return {};
    }
    for (const auto *suffix : {"Name", "DataOrg", "Unit", "Factor", "Precision", "DataHeader", "DataAddr"})
    {
        if (!row.columns.contains(prefix + suffix))
        {
            return invalid(1, prefix + suffix, "missing required axis header");
        }
    }
    int data_header = 0;
    const auto& header = row.get(prefix + "DataHeader");
    if ((!header.empty() && !parse_number(header, data_header)) || data_header < 0)
    {
        return invalid(row.number, prefix + "DataHeader", "expected nonnegative integer");
    }
    if (size > 1)
    {
        const auto precision = scaling_precision(row, prefix + "Factor", prefix + "Precision");
        if (!precision.has_value())
        {
            return std::unexpected(precision.error());
        }
        auto axis = table.append_child("table");
        attribute(axis, "type", type);
        attribute(axis, "name", row.get(prefix + "Name"));
        storage(axis, row.get(prefix + "DataOrg"));
        scaling(axis, row.get(prefix + "Unit"), row.get(prefix + "Factor"), *precision);
    }
    if (data_header > 0)
    {
        const auto address = storage_address(row, prefix + "DataAddr");
        if (!address.has_value())
        {
            return std::unexpected(address.error());
        }
        auto axis = located.append_child("table");
        attribute(axis, "type", type);
        attribute(axis, "storageaddress", *address);
    }
    return {};
}

Status append_map(pugi::xml_node base, pugi::xml_node rom, const MapRow& row)
{
    int sizex = 0;
    int sizey = 0;
    for (const auto& [name, size] : {std::pair{"Columns", &sizex}, std::pair{"Rows", &sizey}})
    {
        if (!parse_number(row.get(name), *size) || *size < 1)
        {
            return invalid(row.number, name, "expected positive dimension");
        }
    }
    const auto precision = scaling_precision(row, "Fieldvalues.Factor", "Precision");
    if (!precision.has_value())
    {
        return std::unexpected(precision.error());
    }
    const auto address = storage_address(row, "Fieldvalues.StartAddr");
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    auto table = base.append_child("table");
    attribute(table, "type", sizex == 1 || sizey == 1 ? "2D" : "3D");
    attribute(table, "name", row.get("Name"));
    attribute(table, "category", row.get("FolderName"));
    storage(table, row.get("DataOrg"));
    attribute(table, "sizex", row.get("Columns"));
    attribute(table, "sizey", row.get("Rows"));
    scaling(table, row.get("Fieldvalues.Name") + " (" + row.get("Fieldvalues.Unit") + ")",
            row.get("Fieldvalues.Factor"), *precision);
    auto located = rom.append_child("table");
    attribute(located, "name", row.get("Name"));
    attribute(located, "storageaddress", *address);
    for (const auto& [prefix, type, size] :
         {std::tuple{"AxisX.", "X Axis", sizex}, std::tuple{"AxisY.", "Y Axis", sizey}})
    {
        if (auto result = append_axis(table, located, row, prefix, type, size); !result.has_value())
        {
            return result;
        }
    }
    if (sizex == 1 && sizey == 1)
    {
        auto axis = table.append_child("table");
        attribute(axis, "type", "Static Y Axis");
        attribute(axis, "name", " ");
        attribute(axis, "sizey", "1");
        axis.append_child("data").text().set("#");
    }
    table.append_child("description").text().set(row.get("Comment").c_str());
    return {};
}
} // namespace

Result<std::string> convert_mappack_csv(std::string_view csv, std::string_view ecu_id)
{
    if (contains_xml_control(ecu_id))
    {
        return invalid(1, "ecu_id", "XML-invalid control character");
    }
    auto parsed = parse_csv(csv);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    auto& rows = *parsed;
    if (rows.size() < 2)
    {
        return invalid(1, "header", "expected header and map rows");
    }
    const auto columns = parse_columns(rows.front());
    if (!columns.has_value())
    {
        return std::unexpected(columns.error());
    }
    pugi::xml_document doc;
    auto roms = doc.append_child("roms");
    auto base = roms.append_child("rom");
    rom_id(base, "BASE");
    auto rom = roms.append_child("rom");
    attribute(rom, "base", "BASE");
    auto id = rom_id(rom, ecu_id);
    id.child("ecuid").text().set(std::string(ecu_id).c_str());
    id.child("internalidaddress").text().set("0x50");
    id.child("internalidstring").text().set("1037369411P321/C51");
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        auto& row = rows[r];
        if (row.size() == columns->size() + 1 && row.back().empty())
        {
            row.pop_back();
        }
        if (row.size() != columns->size())
        {
            return invalid(r + 1, "record", "field count differs from header");
        }
        if (auto result = append_map(base, rom, {row, *columns, r + 1}); !result.has_value())
        {
            return std::unexpected(result.error());
        }
    }
    std::ostringstream output;
    doc.save(output, "    ");
    return output.str();
}
} // namespace fastecu::definition
