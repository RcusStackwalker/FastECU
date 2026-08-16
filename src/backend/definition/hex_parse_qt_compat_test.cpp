// tests/test_hex_parse_qt_compat.cpp
//
// Pins definition::parse_hex_value against the Qt call it replaces on the
// flash path: QString::toUInt(&ok, 16), as used by the former desktop parser.
// Agreement is expected but not assumed -- step 5d-4b's 'g'-formatting
// experience is the reason
// this table exists rather than a comment claiming compatibility.
#include "src/backend/definition/text_format.h"

#include <gtest/gtest.h>

#include <QString>

#include <cstdint>

namespace
{

// Every kernel_addr value in the shipped protocols.cfg. Hard-coded so the
// compatibility oracle is independent of the catalog parser; keep this list
// synchronized when shipped kernel_addr values change.
const char *const kRealKernelAddrs[] = {
    "0x00000000", "0x20000", "0xFFF80000", "0xFFFEE000", "0xFFFF3000", "0xFFFF4000", "0xFFFF6004", "0xFFFF9000",
};

TEST(HexParseQtCompat, AgreesWithQtOnEveryRealKernelAddr)
{
    for (const char *text : kRealKernelAddrs)
    {
        bool ok = false;
        const std::uint32_t qt_value = QString(text).toUInt(&ok, 16);
        ASSERT_TRUE(ok) << "Qt rejected a real kernel_addr: " << text;

        const auto parsed = fastecu::definition::parse_hex_value(text);
        ASSERT_TRUE(parsed.has_value()) << "parse_hex_value rejected: " << text;
        EXPECT_EQ(*parsed, static_cast<std::uint64_t>(qt_value)) << text;
    }
}

// Found in review: a set-literal trim implementation initially only covered
// " \t\r\n" and silently dropped '\v'/'\f', which Qt's toUInt (like
// std::isspace in the C locale, which the original trim_copy relies on)
// accepts as leading/trailing whitespace. Pin agreement on both explicitly
// so this doesn't regress silently again.
TEST(HexParseQtCompat, AgreesWithQtOnVerticalTabAndFormFeed)
{
    for (const char *text : {"\v0x10", "0x10\v", "\f0x10", "0x10\f", "\f0x10\f"})
    {
        bool ok = false;
        const std::uint32_t qt_value = QString(text).toUInt(&ok, 16);
        ASSERT_TRUE(ok) << "Qt rejected: " << text;

        const auto parsed = fastecu::definition::parse_hex_value(text);
        ASSERT_TRUE(parsed.has_value()) << "parse_hex_value rejected: " << text;
        EXPECT_EQ(*parsed, static_cast<std::uint64_t>(qt_value)) << text;
    }
}

TEST(HexParseQtCompat, AgreesWithQtOnRejectionCases)
{
    for (const char *text : {"", "0x", "nonsense", "not_hex", "0xZZZZ"})
    {
        bool ok = true;
        (void)QString(text).toUInt(&ok, 16);
        EXPECT_FALSE(ok) << "Qt unexpectedly accepted: " << text;
        EXPECT_FALSE(fastecu::definition::parse_hex_value(text).has_value()) << text;
    }
}

// Documented divergence, not a bug: parse_hex_value returns uint64 and so
// accepts values Qt's toUInt rejects by overflow. The flash use case narrows
// with an explicit range check (Task 4), which is where the uint32 bound is
// enforced -- so record the divergence here rather than pretending it away.
TEST(HexParseQtCompat, DivergesAboveUint32ByDesign)
{
    bool ok = true;
    (void)QString("0x1FFFFFFFF").toUInt(&ok, 16);
    EXPECT_FALSE(ok); // Qt: overflow

    const auto parsed = fastecu::definition::parse_hex_value("0x1FFFFFFFF");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, 0x1FFFFFFFFull);
}

} // namespace
