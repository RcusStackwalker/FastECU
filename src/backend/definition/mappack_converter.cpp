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

Result<std::vector<Row>> parse_csv(std::string_view csv)
{
    std::vector<Row> rows;
    Row row;
    std::string field;
    bool quoted = false;
    bool closed = false;
    for (std::size_t i = 0; i < csv.size(); ++i)
    {
        const char ch = csv[i];
        if (quoted)
        {
            if (ch == '"')
            {
                if (i + 1 < csv.size() && csv[i + 1] == '"')
                {
                    field += '"';
                    ++i;
                }
                else
                {
                    quoted = false;
                    closed = true;
                }
            }
            else
            {
                field += ch;
            }
        }
        else if (ch == ';' || ch == '\n' || ch == '\r')
        {
            row.push_back(std::move(field));
            field.clear();
            closed = false;
            if (ch != ';')
            {
                if (ch == '\r' && i + 1 < csv.size() && csv[i + 1] == '\n')
                {
                    ++i;
                }
                rows.push_back(std::move(row));
                row.clear();
            }
        }
        else if (ch == '"' && field.empty() && !closed)
        {
            quoted = true;
        }
        else if (closed || ch == '"')
        {
            return invalid(rows.size() + 1, std::to_string(row.size() + 1),
                           "unexpected character after/in quoted field");
        }
        else
        {
            field += ch;
        }
    }
    if (quoted)
    {
        return invalid(rows.size() + 1, std::to_string(row.size() + 1), "unterminated quoted field");
    }
    if (!row.empty() || !field.empty() || closed)
    {
        row.push_back(std::move(field));
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
    std::replace(value.begin(), value.end(), ',', '.');
    return value;
}

void attribute(pugi::xml_node node, const char *name, std::string_view value)
{
    node.append_attribute(name).set_value(value.data(), value.size());
}

void storage(pugi::xml_node node, std::string_view org)
{
    attribute(node, "storagetype", org == "eHiLo" ? "uint16" : org == "eHiLoHiLo" ? "uint32" : "uint8");
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
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        for (std::size_t c = 0; c < rows[r].size(); ++c)
        {
            if (contains_xml_control(rows[r][c]))
            {
                return invalid(r + 1, std::to_string(c + 1), "XML-invalid control character");
            }
        }
    }
    // MapPack exports may terminate each record with an extra semicolon.
    if (rows.front().back().empty())
    {
        rows.front().pop_back();
    }
    std::map<std::string, std::size_t, std::less<>> columns;
    for (std::size_t i = 0; i < rows.front().size(); ++i)
    {
        if (!columns.emplace(rows.front()[i], i).second)
        {
            return invalid(1, rows.front()[i], "duplicate header");
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
        if (row.size() == columns.size() + 1 && row.back().empty())
        {
            row.pop_back();
        }
        if (row.size() != columns.size())
        {
            return invalid(r + 1, "record", "field count differs from header");
        }
        auto get = [&](std::string_view name) -> const std::string& { return row.at(columns.at(std::string(name))); };
        int sizex = 0;
        int sizey = 0;
        for (auto [name, size] : {std::pair{"Columns", &sizex}, std::pair{"Rows", &sizey}})
        {
            if (!parse_number(get(name), *size) || *size < 1)
            {
                return invalid(r + 1, name, "expected positive dimension");
            }
        }
        auto validate_scaling = [&](const std::string& factor_name, const std::string& precision_name) -> Status
        {
            double factor = 0;
            auto text = decimal(get(factor_name));
            auto result = std::from_chars(text.data(), text.data() + text.size(), factor);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(factor) ||
                factor == 0)
            {
                return invalid(r + 1, factor_name, "expected finite nonzero factor");
            }
            int precision = 0;
            if (!parse_number(get(precision_name), precision) || precision < 0 || precision > 100)
            {
                return invalid(r + 1, precision_name, "expected precision between 0 and 100");
            }
            return {};
        };
        auto address = [&](const std::string& name) -> Result<std::string>
        {
            std::string_view text = get(name);
            if (text.starts_with('$'))
            {
                text.remove_prefix(1);
            }
            if (text.starts_with("0x") || text.starts_with("0X"))
            {
                text.remove_prefix(2);
            }
            std::uint64_t value = 0;
            if (!parse_number(text, value, 16))
            {
                return invalid(r + 1, name, "expected hexadecimal address");
            }
            return "0x" + std::string(text);
        };
        auto valid = validate_scaling("Fieldvalues.Factor", "Precision");
        if (!valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        auto addr = address("Fieldvalues.StartAddr");
        if (!addr.has_value())
        {
            return std::unexpected(addr.error());
        }
        auto table = base.append_child("table");
        attribute(table, "type", sizex == 1 || sizey == 1 ? "2D" : "3D");
        attribute(table, "name", get("Name"));
        attribute(table, "category", get("FolderName"));
        storage(table, get("DataOrg"));
        attribute(table, "sizex", get("Columns"));
        attribute(table, "sizey", get("Rows"));
        scaling(table, get("Fieldvalues.Name") + " (" + get("Fieldvalues.Unit") + ")", get("Fieldvalues.Factor"),
                std::stoi(get("Precision")));
        auto located = rom.append_child("table");
        attribute(located, "name", get("Name"));
        attribute(located, "storageaddress", *addr);
        for (auto [prefix, type, size] : {std::tuple{"AxisX.", "X Axis", sizex}, std::tuple{"AxisY.", "Y Axis", sizey}})
        {
            // DataHeader controls address emission independently of axis size in legacy exports.
            const std::string p = prefix;
            if (size <= 1 && !columns.contains(p + "DataHeader"))
            {
                continue;
            }
            for (const auto *suffix : {"Name", "DataOrg", "Unit", "Factor", "Precision", "DataHeader", "DataAddr"})
            {
                if (!columns.contains(p + suffix))
                {
                    return invalid(1, p + suffix, "missing required axis header");
                }
            }
            int data_header = 0;
            if ((!get(p + "DataHeader").empty() && !parse_number(get(p + "DataHeader"), data_header)) ||
                data_header < 0)
            {
                return invalid(r + 1, p + "DataHeader", "expected nonnegative integer");
            }
            if (size > 1)
            {
                valid = validate_scaling(p + "Factor", p + "Precision");
                if (!valid.has_value())
                {
                    return std::unexpected(valid.error());
                }
                auto axis = table.append_child("table");
                attribute(axis, "type", type);
                attribute(axis, "name", get(p + "Name"));
                storage(axis, get(p + "DataOrg"));
                scaling(axis, get(p + "Unit"), get(p + "Factor"), std::stoi(get(p + "Precision")));
            }
            if (data_header > 0)
            {
                addr = address(p + "DataAddr");
                if (!addr.has_value())
                {
                    return std::unexpected(addr.error());
                }
                auto axis = located.append_child("table");
                attribute(axis, "type", type);
                attribute(axis, "storageaddress", *addr);
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
        table.append_child("description").text().set(get("Comment").c_str());
    }
    std::ostringstream output;
    doc.save(output, "    ");
    return output.str();
}
} // namespace fastecu::definition
