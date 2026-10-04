#include "src/backend/config/builtin_catalog.h"

#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pugixml.hpp>

// Temporary: removed with protocols.cfg. Compares the built-in catalog with
// the file it was generated from; every difference must be a documented data
// fix (see the catalog spec's "Data fixes"), listed below in the order the
// fixes were applied.
namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::checksum_flag;
using fastecu::config::kernel_load_address_text;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using ::testing::UnorderedElementsAreArray;

std::string child_text(pugi::xml_node node, const char *tag)
{
    return node.child(tag).text().as_string();
}

std::string yes_no(bool value)
{
    return value ? "yes" : "no";
}

// The catalog keeps only "yes" as true; protocols.cfg also spelled false "n/a".
std::string file_flag(pugi::xml_node node, const char *tag)
{
    return yes_no(child_text(node, tag) == "yes");
}

std::string file_address(pugi::xml_node node)
{
    const std::string text = child_text(node, "kernel_addr");
    return text.empty() ? std::string{} : std::format("0x{:X}", std::stoul(text, nullptr, 16));
}

void compare(std::vector<std::string>& out, std::string_view what, std::string_view field, std::string_view file,
             std::string_view catalog)
{
    if (file != catalog)
    {
        out.push_back(std::format("{}: {} file '{}' catalog '{}'", what, field, file, catalog));
    }
}

std::vector<std::string> protocol_differences(pugi::xml_node protocols)
{
    std::vector<std::string> out;
    for (pugi::xml_node node : protocols.children("protocol"))
    {
        const std::string name = node.attribute("name").as_string();
        const std::string what = std::format("protocol {}", name);
        const ProtocolSpec *spec = builtin_catalog().find_protocol(name);
        if (spec == nullptr)
        {
            out.push_back(what + ": missing from catalog");
            continue;
        }
        compare(out, what, "alias", node.attribute("alias").as_string(), spec->alias);
        compare(out, what, "ecu", child_text(node, "ecu"), spec->ecu);
        compare(out, what, "mcu", child_text(node, "mcu"), spec->mcu);
        compare(out, what, "mode", child_text(node, "mode"), spec->mode);
        compare(out, what, "checksum", child_text(node, "checksum"), checksum_flag(spec->checksum));
        compare(out, what, "read", file_flag(node, "read"), yes_no(spec->read));
        compare(out, what, "test_write", file_flag(node, "test_write"), yes_no(spec->test_write));
        compare(out, what, "write", file_flag(node, "write"), yes_no(spec->write));
        compare(out, what, "flash_transport", child_text(node, "flash_transport"), spec->flash_transport);
        compare(out, what, "log_transport", child_text(node, "log_transport"), spec->log_transport);
        compare(out, what, "log_protocol", child_text(node, "log_protocol"), spec->log_protocol);
        compare(out, what, "kernel", child_text(node, "kernel"), spec->kernel);
        compare(out, what, "kernel_addr", file_address(node), kernel_load_address_text(*spec));
        compare(out, what, "description", child_text(node, "description"), spec->description);
    }
    for (const ProtocolSpec& spec : builtin_catalog().protocols())
    {
        if (!protocols.find_child_by_attribute("protocol", "name", std::string(spec.name).c_str()))
        {
            out.push_back(std::format("protocol {}: not in file", spec.name));
        }
    }
    return out;
}

std::vector<std::string> vehicle_differences(pugi::xml_node car_models)
{
    std::vector<std::string> out;
    const auto vehicles = builtin_catalog().vehicles();
    std::size_t row = 0;
    for (pugi::xml_node node : car_models.children("car_model"))
    {
        const std::string what = std::format("vehicle {}", row);
        if (row >= vehicles.size())
        {
            out.push_back(what + ": missing from catalog");
            ++row;
            continue;
        }
        const VehicleSpec& vehicle = vehicles[row];
        compare(out, what, "make", child_text(node, "make"), vehicle.make);
        compare(out, what, "model", child_text(node, "model"), vehicle.model);
        compare(out, what, "version", child_text(node, "version"), vehicle.version);
        compare(out, what, "type", child_text(node, "type"), vehicle.type);
        compare(out, what, "kw", child_text(node, "kw"), vehicle.kw);
        compare(out, what, "hp", child_text(node, "hp"), vehicle.hp);
        compare(out, what, "fuel", child_text(node, "fuel"), vehicle.fuel);
        compare(out, what, "year", child_text(node, "year"), vehicle.year);
        compare(out, what, "protocol", child_text(node, "protocol"),
                vehicle.protocol != nullptr ? vehicle.protocol->name : std::string_view{});
        ++row;
    }
    for (; row < vehicles.size(); ++row)
    {
        out.push_back(std::format("vehicle {}: not in file ({})", row, vehicles[row].id));
    }
    return out;
}

TEST(BuiltinCatalogParity, DiffersFromProtocolsCfgOnlyByTheDocumentedFixes)
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    ASSERT_NE(path, nullptr);
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_file(path)) << path;
    const pugi::xml_node config = doc.child("config");

    // Fix 1: the bundled file is lowercase; case-sensitive filesystems failed the read.
    const std::string fix_1_kernel_diff = "protocol sub_tcu_denso_sh7055_can: kernel file 'ssmk_tcu_can_SH7055_35.bin' "
                                          "catalog 'ssmk_tcu_can_sh7055_35.bin'";
    const std::vector<std::string> expected_protocol_differences{
        fix_1_kernel_diff,
        // Fix 4: no kernel, so no load address; the family's plan requires none.
        "protocol sub_ecu_unisia_jecs_m3779x: kernel_addr file '0x0' catalog ''",
        "protocol sub_ecu_unisia_jecs_m3775x: kernel_addr file '0x0' catalog ''",
        // Fix 5: the family talks to the on-board kernel and rejects a plan with one;
        // the named kernel was never bundled.
        "protocol sub_ecu_denso_sh72543_can_diesel: kernel file 'ssmk_can_tp_sh72543d_euro6.bin' catalog ''",
        "protocol sub_ecu_denso_sh72543_can_diesel: kernel_addr file '0xFFF80000' catalog ''",
        // Fix 6: 0xFFFF6004 is the SH7055 address; the same kernel loads at
        // 0xFFFF3000 for every main-flash SH7058 DensoCAN entry. VERIFY on bench.
        "protocol sub_ecu_eeprom_denso_sh7058_densocan: kernel_addr file '0xFFFF6004' catalog '0xFFFF3000'",
        // Fix 7: revision 04 declared no supported operation.
        "protocol sub_ecu_denso_mc68hc16y5_04: missing from catalog",
        "protocol sub_ecu_denso_mc68hc16y5_04_ecutek: missing from catalog",
    };
    const std::vector<std::string> expected_vehicle_differences{
        // Fix 2: upstream 90f11ae9 renamed these protocols without updating the vehicles.
        "vehicle 1: protocol file 'sub_ecu_unisia_jecs_92' catalog 'sub_ecu_unisia_jecs_m3779x'",
        "vehicle 2: protocol file 'sub_ecu_unisia_jecs_97' catalog 'sub_ecu_unisia_jecs_m3775x'",
        // Fix 3: typos.
        "vehicle 10: version file '2.0 5MT ' catalog '2.0 5MT'",
        "vehicle 35: year file '20011' catalog '2011'",
        "vehicle 39: version file '2.0 5MT ' catalog '2.0 5MT'",
        "vehicle 40: version file '2.0 5MT ' catalog '2.0 5MT'",
        // Fix 8: one vehicle for each protocol no vehicle reached.
        "vehicle 65: not in file (mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma)",
        "vehicle 66: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-02-ecutek)",
        "vehicle 67: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-04-cobb)",
        "vehicle 68: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-cobb)",
        "vehicle 69: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-can-cobb)",
        "vehicle 70: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-kline)",
        "vehicle 71: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-kline)",
        "vehicle 72: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-densocan)",
        "vehicle 73: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-densocan)",
        "vehicle 74: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-can-diesel)",
    };

    EXPECT_THAT(protocol_differences(config.child("protocols")),
                UnorderedElementsAreArray(expected_protocol_differences));
    EXPECT_THAT(vehicle_differences(config.child("car_models")),
                UnorderedElementsAreArray(expected_vehicle_differences));
}

} // namespace
