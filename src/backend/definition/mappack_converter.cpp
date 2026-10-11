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

bool ContainsXmlControl(std::string_view text)
{
    return std::ranges::any_of(text,
                               [](unsigned char ch) { return ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r'; });
}

std::unexpected<Error> Invalid(std::size_t row, std::string_view column, std::string_view reason)
{
    return Fail(ErrorKind::kInvalidConfig, std::format("CSV row {}, column {}: {}", row, column, reason));
}

// Consume one field, leaving the record separator for the caller.
Result<std::string> ParseField(std::string_view& csv, std::size_t row, std::size_t column)
{
    const auto error = [&](std::string_view reason) { return Invalid(row, std::to_string(column), reason); };
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

Result<std::vector<Row>> ParseCsv(std::string_view csv)
{
    std::vector<Row> rows;
    Row row;
    while (!csv.empty())
    {
        auto field = ParseField(csv, rows.size() + 1, row.size() + 1);
        if (!field.has_value())
        {
            return std::unexpected(field.error());
        }
        if (ContainsXmlControl(*field))
        {
            return Invalid(rows.size() + 1, std::to_string(row.size() + 1), "XML-invalid control character");
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

template <typename T> bool ParseNumber(std::string_view text, T& value, int base = 10)
{
    if (text.empty())
    {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string Decimal(std::string value)
{
    std::ranges::replace(value, ',', '.');
    return value;
}

void Attribute(pugi::xml_node node, const char *name, std::string_view value)
{
    node.append_attribute(name).set_value(value.data(), value.size());
}

void Storage(pugi::xml_node node, std::string_view org)
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
    Attribute(node, "storagetype", type);
    Attribute(node, "endian", "big");
}

void Scaling(pugi::xml_node table, std::string_view units, std::string factor, int precision)
{
    auto node = table.append_child("scaling");
    Attribute(node, "units", units);
    factor = Decimal(std::move(factor));
    Attribute(node, "expression", "x*" + factor);
    Attribute(node, "to_byte", "x/" + factor);
    Attribute(node, "format", precision == 0 ? "#" : "#0." + std::string(precision, '0'));
    Attribute(node, "fineincrement", "1");
    Attribute(node, "coarseincrement", "1");
}

pugi::xml_node RomId(pugi::xml_node rom, std::string_view id)
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

Result<Columns> ParseColumns(Row& header)
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
            return Invalid(1, header[i], "duplicate header");
        }
    }
    for (const auto *name : {"Name", "FolderName", "DataOrg", "Columns", "Rows", "Fieldvalues.Name", "Fieldvalues.Unit",
                             "Fieldvalues.Factor", "Precision", "Fieldvalues.StartAddr", "Comment"})
    {
        if (!columns.contains(name))
        {
            return Invalid(1, name, "missing required header");
        }
    }
    return columns;
}

struct MapRow
{
    const Row& fields;
    const Columns& columns;
    std::size_t number;

    const std::string& Get(std::string_view name) const
    {
        return fields.at(columns.find(name)->second);
    }
};

Result<int> ScalingPrecision(const MapRow& row, std::string_view factor_name, std::string_view precision_name)
{
    double factor = 0;
    const auto text = Decimal(row.Get(factor_name));
    if (const auto result = std::from_chars(text.data(), text.data() + text.size(), factor);
        result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(factor) || factor == 0)
    {
        return Invalid(row.number, factor_name, "expected finite nonzero factor");
    }
    int precision = 0;
    if (!ParseNumber(row.Get(precision_name), precision) || precision < 0 || precision > 100)
    {
        return Invalid(row.number, precision_name, "expected precision between 0 and 100");
    }
    return precision;
}

Result<std::string> StorageAddress(const MapRow& row, std::string_view name)
{
    std::string_view text = row.Get(name);
    if (text.starts_with('$'))
    {
        text.remove_prefix(1);
    }
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }
    if (std::uint64_t value = 0; !ParseNumber(text, value, 16))
    {
        return Invalid(row.number, name, "expected hexadecimal address");
    }
    return "0x" + std::string(text);
}

Status AppendAxis(pugi::xml_node table, pugi::xml_node located, const MapRow& row, const std::string& prefix,
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
            return Invalid(1, prefix + suffix, "missing required axis header");
        }
    }
    int data_header = 0;
    const auto& header = row.Get(prefix + "DataHeader");
    if ((!header.empty() && !ParseNumber(header, data_header)) || data_header < 0)
    {
        return Invalid(row.number, prefix + "DataHeader", "expected nonnegative integer");
    }
    if (size > 1)
    {
        const auto precision = ScalingPrecision(row, prefix + "Factor", prefix + "Precision");
        if (!precision.has_value())
        {
            return std::unexpected(precision.error());
        }
        auto axis = table.append_child("table");
        Attribute(axis, "type", type);
        Attribute(axis, "name", row.Get(prefix + "Name"));
        Storage(axis, row.Get(prefix + "DataOrg"));
        Scaling(axis, row.Get(prefix + "Unit"), row.Get(prefix + "Factor"), *precision);
    }
    if (data_header > 0)
    {
        const auto address = StorageAddress(row, prefix + "DataAddr");
        if (!address.has_value())
        {
            return std::unexpected(address.error());
        }
        auto axis = located.append_child("table");
        Attribute(axis, "type", type);
        Attribute(axis, "storageaddress", *address);
    }
    return {};
}

Status AppendMap(pugi::xml_node base, pugi::xml_node rom, const MapRow& row)
{
    int sizex = 0;
    int sizey = 0;
    for (const auto& [name, size] : {std::pair{"Columns", &sizex}, std::pair{"Rows", &sizey}})
    {
        if (!ParseNumber(row.Get(name), *size) || *size < 1)
        {
            return Invalid(row.number, name, "expected positive dimension");
        }
    }
    const auto precision = ScalingPrecision(row, "Fieldvalues.Factor", "Precision");
    if (!precision.has_value())
    {
        return std::unexpected(precision.error());
    }
    const auto address = StorageAddress(row, "Fieldvalues.StartAddr");
    if (!address.has_value())
    {
        return std::unexpected(address.error());
    }
    auto table = base.append_child("table");
    Attribute(table, "type", sizex == 1 || sizey == 1 ? "2D" : "3D");
    Attribute(table, "name", row.Get("Name"));
    Attribute(table, "category", row.Get("FolderName"));
    Storage(table, row.Get("DataOrg"));
    Attribute(table, "sizex", row.Get("Columns"));
    Attribute(table, "sizey", row.Get("Rows"));
    Scaling(table, row.Get("Fieldvalues.Name") + " (" + row.Get("Fieldvalues.Unit") + ")",
            row.Get("Fieldvalues.Factor"), *precision);
    auto located = rom.append_child("table");
    Attribute(located, "name", row.Get("Name"));
    Attribute(located, "storageaddress", *address);
    for (const auto& [prefix, type, size] :
         {std::tuple{"AxisX.", "X Axis", sizex}, std::tuple{"AxisY.", "Y Axis", sizey}})
    {
        if (auto result = AppendAxis(table, located, row, prefix, type, size); !result.has_value())
        {
            return result;
        }
    }
    if (sizex == 1 && sizey == 1)
    {
        auto axis = table.append_child("table");
        Attribute(axis, "type", "Static Y Axis");
        Attribute(axis, "name", " ");
        Attribute(axis, "sizey", "1");
        axis.append_child("data").text().set("#");
    }
    table.append_child("description").text().set(row.Get("Comment").c_str());
    return {};
}
} // namespace

Result<std::string> ConvertMappackCsv(std::string_view csv, std::string_view ecu_id)
{
    if (ContainsXmlControl(ecu_id))
    {
        return Invalid(1, "ecu_id", "XML-invalid control character");
    }
    auto parsed = ParseCsv(csv);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    auto& rows = *parsed;
    if (rows.size() < 2)
    {
        return Invalid(1, "header", "expected header and map rows");
    }
    const auto columns = ParseColumns(rows.front());
    if (!columns.has_value())
    {
        return std::unexpected(columns.error());
    }
    pugi::xml_document doc;
    auto roms = doc.append_child("roms");
    auto base = roms.append_child("rom");
    RomId(base, "BASE");
    auto rom = roms.append_child("rom");
    Attribute(rom, "base", "BASE");
    auto id = RomId(rom, ecu_id);
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
            return Invalid(r + 1, "record", "field count differs from header");
        }
        if (auto result = AppendMap(base, rom, {row, *columns, r + 1}); !result.has_value())
        {
            return std::unexpected(result.error());
        }
    }
    std::ostringstream output;
    doc.save(output, "    ");
    return output.str();
}
} // namespace fastecu::definition
