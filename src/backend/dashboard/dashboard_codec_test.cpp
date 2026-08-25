#include "src/backend/dashboard/dashboard_codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/dashboard/test_fixtures.h"

namespace
{
using namespace fastecu;
using namespace fastecu::dashboard;

constexpr std::string_view kCanonicalXml = R"(<?xml version="1.0" encoding="utf-8"?>
<omnihaste-dashboard format-version="1">
    <metadata name="Colt Dashboard" description="Example"/>
    <connection protocol="cdbg" transport="raw-can" bitrate="500000" identifier-width="11" request-id="0x630" reply-id="0x631" stream-instance="0" sampling-interval-ms="50">
        <retry poll-timeout-ms="100" silence-threshold="3" reconnect-attempts="3" reconnect-period-ms="250"/>
    </connection>
    <channels>
        <channel id="CDBG_ENGINE_RPM" name="Engine RPM" description="engine_rpm uint16" address="0x804cfc" length="2" raw-assembly="unsigned-integer-decimal">
            <conversion id="conversion-1" expression="x*1000/256" unit="rpm" precision="0" gauge-min="0" gauge-max="8000" gauge-step="500"/>
        </channel>
    </channels>
    <cards/>
</omnihaste-dashboard>
)";

bytes::ByteView byte_view(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::string replace_once(std::string input, std::string_view before, std::string_view after)
{
    const std::size_t position = input.find(before);
    EXPECT_NE(position, std::string::npos) << before;
    if (position != std::string::npos)
    {
        input.replace(position, before.size(), after);
    }
    return input;
}

std::string with_card(std::string_view attributes)
{
    return replace_once(std::string(kCanonicalXml), "    <cards/>",
                        "    <cards>\n        <card " + std::string(attributes) + "/>\n    </cards>");
}

std::string with_adapter(std::string_view attributes)
{
    return replace_once(std::string(kCanonicalXml),
                        "        <retry poll-timeout-ms=\"100\" silence-threshold=\"3\" "
                        "reconnect-attempts=\"3\" reconnect-period-ms=\"250\"/>",
                        "        <retry poll-timeout-ms=\"100\" silence-threshold=\"3\" reconnect-attempts=\"3\" "
                        "reconnect-period-ms=\"250\"/>\n        <preferred-adapter " +
                            std::string(attributes) + "/>");
}

void expect_decode_error(std::string_view xml, ErrorKind kind, std::string_view path)
{
    const auto result = decode_dashboard_document(byte_view(xml));
    ASSERT_FALSE(result) << xml;
    EXPECT_EQ(result.error().kind, kind) << result.error().detail;
    EXPECT_TRUE(result.error().detail.starts_with(path)) << result.error().detail;
}

void expect_encode_string_error(const DashboardDocument& document, std::string_view path)
{
    const auto result = encode_dashboard_document(document);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(result.error().detail, std::string(path) + ": must contain only valid UTF-8 XML 1.0 characters");
}

DashboardDocument document_with_all_optional_strings()
{
    auto document = test::valid_document();
    document.connection.preferred_adapter = PreferredAdapter{AdapterKind::J2534, "OpenECU", "Portable CAN"};
    document.cards = {
        DashboardCard{"rpm", "CDBG_ENGINE_RPM", "conversion-1", CardDisplayType::Numeric, "Engine RPM", 0, std::nullopt,
                      std::nullopt},
    };
    return document;
}

struct XmlCase
{
    std::string xml;
    std::string_view path;
};
} // namespace

TEST(DashboardCodec, DecodesEncodesAndSemanticallyRoundTripsTheCanonicalDocument)
{
    const auto decoded = decode_dashboard_document(byte_view(kCanonicalXml));
    ASSERT_TRUE(decoded) << decoded.error().detail;
    EXPECT_EQ(*decoded, test::valid_document());

    const auto encoded = encode_dashboard_document(test::valid_document());
    ASSERT_TRUE(encoded) << encoded.error().detail;
    EXPECT_EQ(std::string(encoded->begin(), encoded->end()), kCanonicalXml);

    const auto reparsed = decode_dashboard_document(*encoded);
    ASSERT_TRUE(reparsed) << reparsed.error().detail;
    EXPECT_EQ(*reparsed, test::valid_document());
    EXPECT_EQ(encode_dashboard_document(*reparsed), encoded);
}

TEST(DashboardCodec, RejectsMalformedMissingAndWrongRoots)
{
    expect_decode_error("<omnihaste-dashboard", ErrorKind::InvalidConfig, "document");
    expect_decode_error("<?xml version=\"1.0\"?>", ErrorKind::InvalidConfig, "document");
    expect_decode_error("<dashboard format-version=\"1\"/>", ErrorKind::InvalidConfig, "document.root");
}

TEST(DashboardCodec, RejectsASecondTopLevelElement)
{
    expect_decode_error(std::string(kCanonicalXml) + "<second-root/>", ErrorKind::InvalidConfig, "document.root");
}

TEST(DashboardCodec, DispatchesOnlyAfterParsingAnAuthoritativeNumericVersion)
{
    expect_decode_error("<omnihaste-dashboard/>", ErrorKind::InvalidConfig, "metadata.format-version");
    expect_decode_error("<omnihaste-dashboard format-version=\"one\"/>", ErrorKind::InvalidConfig,
                        "metadata.format-version");
    expect_decode_error("<omnihaste-dashboard format-version=\"2\"/>", ErrorKind::Unsupported,
                        "metadata.format-version");
    expect_decode_error("<omnihaste-dashboard format-version=\"4294967296\"/>", ErrorKind::InvalidConfig,
                        "metadata.format-version");
}

TEST(DashboardCodec, EncodingValidatesBeforeWriting)
{
    auto document = test::valid_document();
    document.metadata.format_version = 2;
    const auto result = encode_dashboard_document(document);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(result.error().detail.starts_with("metadata.format-version")) << result.error().detail;
}

TEST(DashboardCodec, EncodingRejectsEmbeddedNulAtTheExactFieldPath)
{
    auto document = test::valid_document();
    document.metadata.name = std::string("Colt\0Dashboard", 14);
    expect_encode_string_error(document, "metadata.name");
}

TEST(DashboardCodec, EncodingRejectsInvalidUtf8AtEverySerializedStringFieldPath)
{
    struct StringCase
    {
        void (*mutate)(DashboardDocument&, const std::string&);
        std::string_view path;
    };
    const std::array cases{
        StringCase{[](DashboardDocument& document, const std::string& invalid) { document.metadata.name = invalid; },
                   "metadata.name"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.metadata.description = invalid; }, "metadata.description"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.connection.preferred_adapter->vendor = invalid; }, "connection.preferred-adapter.vendor"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.connection.preferred_adapter->display_name = invalid; },
                   "connection.preferred-adapter.display-name"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   {
                       document.channels.front().id = invalid;
                       document.cards.front().channel_id = invalid;
                   },
                   "channels[0].id"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.channels.front().name = invalid; }, "channels[CDBG_ENGINE_RPM].name"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.channels.front().description = invalid; }, "channels[CDBG_ENGINE_RPM].description"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   {
                       document.channels.front().conversions.front().id = invalid;
                       document.cards.front().conversion_id = invalid;
                   },
                   "channels[CDBG_ENGINE_RPM].conversions[0].id"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.channels.front().conversions.front().expression = invalid; },
                   "channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.channels.front().conversions.front().unit = invalid; },
                   "channels[CDBG_ENGINE_RPM].conversions[conversion-1].unit"},
        StringCase{[](DashboardDocument& document, const std::string& invalid) { document.cards.front().id = invalid; },
                   "cards[0].id"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.cards.front().channel_id = invalid; }, "cards[rpm].channel-id"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.cards.front().conversion_id = invalid; }, "cards[rpm].conversion-id"},
        StringCase{[](DashboardDocument& document, const std::string& invalid)
                   { document.cards.front().title = invalid; }, "cards[rpm].title"},
    };
    const std::string invalid_utf8 = "broken" + std::string("\xc3\x28", 2);
    for (const auto& test_case : cases)
    {
        auto document = document_with_all_optional_strings();
        test_case.mutate(document, invalid_utf8);
        expect_encode_string_error(document, test_case.path);
    }
}

TEST(DashboardCodec, DecodingRejectsEmbeddedNulAndInvalidUtf8BeforeParsing)
{
    std::string nul_terminated = std::string(kCanonicalXml);
    nul_terminated.push_back('\0');
    nul_terminated.append("<ignored-root/>");
    expect_decode_error(nul_terminated, ErrorKind::InvalidConfig, "document");

    const std::string invalid_utf8 = replace_once(std::string(kCanonicalXml), "name=\"Colt Dashboard\"",
                                                  "name=\"Colt " + std::string("\xc3\x28", 2) + " Dashboard\"");
    expect_decode_error(invalid_utf8, ErrorKind::InvalidConfig, "document");
}

TEST(DashboardCodec, DecodingRejectsAForbiddenXmlCharacterReferenceAtTheDocumentPath)
{
    const std::string invalid_character =
        replace_once(std::string(kCanonicalXml), "name=\"Colt Dashboard\"", "name=\"Colt&#0;Dashboard\"");
    expect_decode_error(invalid_character, ErrorKind::InvalidConfig, "document");
}

TEST(DashboardCodec, DecodingIgnoresLiteralCharacterReferenceTextInSurroundingComments)
{
    std::string xml = replace_once(std::string(kCanonicalXml), "?>\n", "?>\n<!-- &#0; -->\n");
    xml.append("<!-- &#0; -->\n");
    const auto decoded = decode_dashboard_document(byte_view(xml));
    ASSERT_TRUE(decoded) << decoded.error().detail;
    EXPECT_EQ(*decoded, test::valid_document());
}

TEST(DashboardCodec, DecodingIgnoresLiteralCharacterReferenceTextInSurroundingProcessingInstructions)
{
    std::string xml = replace_once(std::string(kCanonicalXml), "?>\n", "?>\n<?probe &#0;?>\n");
    xml.append("<?probe &#0;?>\n");
    const auto decoded = decode_dashboard_document(byte_view(xml));
    ASSERT_TRUE(decoded) << decoded.error().detail;
    EXPECT_EQ(*decoded, test::valid_document());
}

TEST(DashboardCodec, RequiresEverySingletonSectionExactlyOnce)
{
    const std::array cases{
        XmlCase{replace_once(std::string(kCanonicalXml),
                             "    <metadata name=\"Colt Dashboard\" description=\"Example\"/>\n", ""),
                "metadata"},
        XmlCase{replace_once(std::string(kCanonicalXml),
                             "    <metadata name=\"Colt Dashboard\" description=\"Example\"/>",
                             "    <metadata name=\"Colt Dashboard\" description=\"Example\"/>\n"
                             "    <metadata name=\"Duplicate\"/>"),
                "metadata"},
        XmlCase{replace_once(std::string(kCanonicalXml),
                             "    <connection protocol=\"cdbg\" transport=\"raw-can\" bitrate=\"500000\" "
                             "identifier-width=\"11\" request-id=\"0x630\" reply-id=\"0x631\" "
                             "stream-instance=\"0\" sampling-interval-ms=\"50\">\n"
                             "        <retry poll-timeout-ms=\"100\" silence-threshold=\"3\" "
                             "reconnect-attempts=\"3\" reconnect-period-ms=\"250\"/>\n"
                             "    </connection>\n",
                             ""),
                "connection"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    </connection>", "    </connection>\n    <connection/>"),
                "connection"},
        XmlCase{replace_once(std::string(kCanonicalXml),
                             "    <channels>\n"
                             "        <channel id=\"CDBG_ENGINE_RPM\" name=\"Engine RPM\" "
                             "description=\"engine_rpm uint16\" address=\"0x804cfc\" length=\"2\" "
                             "raw-assembly=\"unsigned-integer-decimal\">\n"
                             "            <conversion id=\"conversion-1\" expression=\"x*1000/256\" unit=\"rpm\" "
                             "precision=\"0\" gauge-min=\"0\" gauge-max=\"8000\" gauge-step=\"500\"/>\n"
                             "        </channel>\n"
                             "    </channels>\n",
                             ""),
                "channels"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    </channels>", "    </channels>\n    <channels/>"),
                "channels"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    <cards/>\n", ""), "cards"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    <cards/>", "    <cards/>\n    <cards/>"), "cards"},
        XmlCase{replace_once(std::string(kCanonicalXml),
                             "        <retry poll-timeout-ms=\"100\" silence-threshold=\"3\" "
                             "reconnect-attempts=\"3\" reconnect-period-ms=\"250\"/>\n",
                             ""),
                "connection.retry"},
        XmlCase{replace_once(std::string(kCanonicalXml), "        <retry poll-timeout-ms=\"100\"",
                             "        <retry poll-timeout-ms=\"100\" silence-threshold=\"3\" "
                             "reconnect-attempts=\"3\" reconnect-period-ms=\"250\"/>\n"
                             "        <retry poll-timeout-ms=\"100\""),
                "connection.retry"},
        XmlCase{replace_once(with_adapter("kind=\"j2534\" vendor=\"OpenECU\" display-name=\"CAN\""),
                             "        <preferred-adapter kind=\"j2534\" vendor=\"OpenECU\" "
                             "display-name=\"CAN\"/>",
                             "        <preferred-adapter kind=\"j2534\" vendor=\"OpenECU\" "
                             "display-name=\"CAN\"/>\n"
                             "        <preferred-adapter kind=\"j2534\" vendor=\"OpenECU\" "
                             "display-name=\"CAN\"/>"),
                "connection.preferred-adapter"},
    };

    for (const auto& test_case : cases)
    {
        expect_decode_error(test_case.xml, ErrorKind::InvalidConfig, test_case.path);
    }
}

TEST(DashboardCodec, RejectsUnknownElementsAtEveryV1NestingLevel)
{
    const std::array cases{
        XmlCase{replace_once(std::string(kCanonicalXml), "    <cards/>", "    <mystery/>\n    <cards/>"),
                "document.root"},
        XmlCase{replace_once(std::string(kCanonicalXml), " description=\"Example\"/>",
                             " description=\"Example\"><mystery/></metadata>"),
                "metadata"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    </connection>", "        <mystery/>\n    </connection>"),
                "connection"},
        XmlCase{replace_once(std::string(kCanonicalXml), " reconnect-period-ms=\"250\"/>",
                             " reconnect-period-ms=\"250\"><mystery/></retry>"),
                "connection.retry"},
        XmlCase{replace_once(with_adapter("kind=\"j2534\" vendor=\"OpenECU\" display-name=\"CAN\""),
                             " display-name=\"CAN\"/>", " display-name=\"CAN\"><mystery/></preferred-adapter>"),
                "connection.preferred-adapter"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    </channels>", "        <mystery/>\n    </channels>"),
                "channels"},
        XmlCase{replace_once(std::string(kCanonicalXml), "        </channel>",
                             "            <mystery/>\n        </channel>"),
                "channels[CDBG_ENGINE_RPM]"},
        XmlCase{replace_once(std::string(kCanonicalXml), " gauge-step=\"500\"/>",
                             " gauge-step=\"500\"><mystery/></conversion>"),
                "channels[CDBG_ENGINE_RPM].conversions[conversion-1]"},
        XmlCase{replace_once(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                       "display-type=\"numeric\" order=\"0\""),
                             "    </cards>", "        <mystery/>\n    </cards>"),
                "cards"},
        XmlCase{replace_once(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                       "display-type=\"numeric\" order=\"0\""),
                             " order=\"0\"/>", " order=\"0\"><mystery/></card>"),
                "cards[rpm]"},
    };

    for (const auto& test_case : cases)
    {
        expect_decode_error(test_case.xml, ErrorKind::InvalidConfig, test_case.path);
    }
}

TEST(DashboardCodec, RejectsUnknownAttributesOnEveryV1Element)
{
    const std::array cases{
        XmlCase{replace_once(std::string(kCanonicalXml), " format-version=\"1\"", " format-version=\"1\" extra=\"x\""),
                "document.root"},
        XmlCase{replace_once(std::string(kCanonicalXml), " name=\"Colt Dashboard\"",
                             " name=\"Colt Dashboard\" extra=\"x\""),
                "metadata"},
        XmlCase{replace_once(std::string(kCanonicalXml), " protocol=\"cdbg\"", " protocol=\"cdbg\" extra=\"x\""),
                "connection"},
        XmlCase{replace_once(std::string(kCanonicalXml), " poll-timeout-ms=\"100\"",
                             " poll-timeout-ms=\"100\" extra=\"x\""),
                "connection.retry"},
        XmlCase{replace_once(with_adapter("kind=\"j2534\" vendor=\"OpenECU\" display-name=\"CAN\""), " kind=\"j2534\"",
                             " kind=\"j2534\" extra=\"x\""),
                "connection.preferred-adapter"},
        XmlCase{replace_once(std::string(kCanonicalXml), "    <channels>", "    <channels extra=\"x\">"), "channels"},
        XmlCase{
            replace_once(std::string(kCanonicalXml), " id=\"CDBG_ENGINE_RPM\"", " id=\"CDBG_ENGINE_RPM\" extra=\"x\""),
            "channels[CDBG_ENGINE_RPM]"},
        XmlCase{replace_once(std::string(kCanonicalXml), " id=\"conversion-1\"", " id=\"conversion-1\" extra=\"x\""),
                "channels[CDBG_ENGINE_RPM].conversions[conversion-1]"},
        XmlCase{replace_once(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                       "display-type=\"numeric\" order=\"0\""),
                             "    <cards>", "    <cards extra=\"x\">"),
                "cards"},
        XmlCase{replace_once(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                       "display-type=\"numeric\" order=\"0\""),
                             " id=\"rpm\"", " id=\"rpm\" extra=\"x\""),
                "cards[rpm]"},
    };

    for (const auto& test_case : cases)
    {
        expect_decode_error(test_case.xml, ErrorKind::InvalidConfig, test_case.path);
    }
}

TEST(DashboardCodec, RequiresEveryV1Attribute)
{
    struct AttributeCase
    {
        std::string_view token;
        std::string_view path;
    };
    constexpr std::array cases{
        AttributeCase{" name=\"Colt Dashboard\"", "metadata.name"},
        AttributeCase{" protocol=\"cdbg\"", "connection.protocol"},
        AttributeCase{" transport=\"raw-can\"", "connection.transport"},
        AttributeCase{" bitrate=\"500000\"", "connection.bitrate"},
        AttributeCase{" identifier-width=\"11\"", "connection.identifier-width"},
        AttributeCase{" request-id=\"0x630\"", "connection.request-id"},
        AttributeCase{" reply-id=\"0x631\"", "connection.reply-id"},
        AttributeCase{" stream-instance=\"0\"", "connection.stream-instance"},
        AttributeCase{" sampling-interval-ms=\"50\"", "connection.sampling-interval-ms"},
        AttributeCase{" poll-timeout-ms=\"100\"", "connection.retry.poll-timeout-ms"},
        AttributeCase{" silence-threshold=\"3\"", "connection.retry.silence-threshold"},
        AttributeCase{" reconnect-attempts=\"3\"", "connection.retry.reconnect-attempts"},
        AttributeCase{" reconnect-period-ms=\"250\"", "connection.retry.reconnect-period-ms"},
        AttributeCase{" id=\"CDBG_ENGINE_RPM\"", "channels[0].id"},
        AttributeCase{" name=\"Engine RPM\"", "channels[CDBG_ENGINE_RPM].name"},
        AttributeCase{" description=\"engine_rpm uint16\"", "channels[CDBG_ENGINE_RPM].description"},
        AttributeCase{" address=\"0x804cfc\"", "channels[CDBG_ENGINE_RPM].address"},
        AttributeCase{" length=\"2\"", "channels[CDBG_ENGINE_RPM].length"},
        AttributeCase{" raw-assembly=\"unsigned-integer-decimal\"", "channels[CDBG_ENGINE_RPM].raw-assembly"},
        AttributeCase{" id=\"conversion-1\"", "channels[CDBG_ENGINE_RPM].conversions[0].id"},
        AttributeCase{" expression=\"x*1000/256\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].expression"},
        AttributeCase{" unit=\"rpm\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].unit"},
        AttributeCase{" precision=\"0\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].precision"},
        AttributeCase{" gauge-min=\"0\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].gauge-min"},
        AttributeCase{" gauge-max=\"8000\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].gauge-max"},
        AttributeCase{" gauge-step=\"500\"", "channels[CDBG_ENGINE_RPM].conversions[conversion-1].gauge-step"},
    };

    for (const auto& test_case : cases)
    {
        expect_decode_error(replace_once(std::string(kCanonicalXml), test_case.token, ""), ErrorKind::InvalidConfig,
                            test_case.path);
    }

    for (const AttributeCase test_case :
         {AttributeCase{" kind=\"j2534\"", "connection.preferred-adapter.kind"},
          AttributeCase{" vendor=\"OpenECU\"", "connection.preferred-adapter.vendor"},
          AttributeCase{" display-name=\"CAN\"", "connection.preferred-adapter.display-name"}})
    {
        expect_decode_error(
            replace_once(with_adapter("kind=\"j2534\" vendor=\"OpenECU\" display-name=\"CAN\""), test_case.token, ""),
            ErrorKind::InvalidConfig, test_case.path);
    }

    constexpr std::string_view card_attributes =
        "id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" display-type=\"numeric\" "
        "order=\"0\"";
    for (const AttributeCase test_case : {AttributeCase{" id=\"rpm\"", "cards[0].id"},
                                          AttributeCase{" channel-id=\"CDBG_ENGINE_RPM\"", "cards[rpm].channel-id"},
                                          AttributeCase{" conversion-id=\"conversion-1\"", "cards[rpm].conversion-id"},
                                          AttributeCase{" display-type=\"numeric\"", "cards[rpm].display-type"},
                                          AttributeCase{" order=\"0\"", "cards[rpm].order"}})
    {
        expect_decode_error(replace_once(with_card(card_attributes), test_case.token, ""), ErrorKind::InvalidConfig,
                            test_case.path);
    }
}

TEST(DashboardCodec, ParsesBothAdapterKindsAndEscapedUtf8Strings)
{
    for (const auto [spelling, expected] :
         {std::pair{"j2534", AdapterKind::J2534}, std::pair{"socketcan", AdapterKind::SocketCan}})
    {
        auto xml = with_adapter("kind=\"" + std::string(spelling) +
                                "\" vendor=\"M&amp;M Ω\" "
                                "display-name=\"CAN &lt;USB&gt;\"");
        xml = replace_once(std::move(xml), "name=\"Colt Dashboard\" description=\"Example\"",
                           "name=\"M&amp;M Ω\" description=\"Coolant &lt;100°C\"");
        const auto result = decode_dashboard_document(byte_view(xml));
        ASSERT_TRUE(result) << result.error().detail;
        ASSERT_TRUE(result->connection.preferred_adapter);
        EXPECT_EQ(result->connection.preferred_adapter->kind, expected);
        EXPECT_EQ(result->connection.preferred_adapter->vendor, "M&M Ω");
        EXPECT_EQ(result->connection.preferred_adapter->display_name, "CAN <USB>");
        EXPECT_EQ(result->metadata.name, "M&M Ω");
        EXPECT_EQ(result->metadata.description, "Coolant <100°C");
    }
}

TEST(DashboardCodec, ParsesAllThreeCardTypesAndTheirOptionalFields)
{
    struct CardCase
    {
        std::string_view attributes;
        CardDisplayType expected;
    };
    constexpr std::array cases{
        CardCase{"id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                 "display-type=\"numeric\" title=\"RPM &amp; speed\" order=\"0\"",
                 CardDisplayType::Numeric},
        CardCase{"id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                 "display-type=\"sparkline\" order=\"0\" sparkline-history-seconds=\"60\"",
                 CardDisplayType::Sparkline},
        CardCase{"id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                 "display-type=\"horizontal-gauge\" order=\"0\" gauge-min=\"0\" gauge-max=\"9000\" "
                 "gauge-step=\"250\"",
                 CardDisplayType::HorizontalGauge},
    };

    for (const auto& test_case : cases)
    {
        const auto result = decode_dashboard_document(byte_view(with_card(test_case.attributes)));
        ASSERT_TRUE(result) << result.error().detail;
        ASSERT_EQ(result->cards.size(), 1);
        EXPECT_EQ(result->cards.front().display_type, test_case.expected);
    }
}

TEST(DashboardCodec, ParsesStandardAndExtendedCanonicalHexIdentifiers)
{
    auto standard = decode_dashboard_document(byte_view(kCanonicalXml));
    ASSERT_TRUE(standard) << standard.error().detail;
    EXPECT_EQ(standard->connection.identifier_width, CanIdentifierWidth::Standard);
    EXPECT_EQ(standard->connection.request_id, 0x630u);
    EXPECT_EQ(standard->channels.front().address, 0x804cfcu);

    auto extended_xml = replace_once(std::string(kCanonicalXml),
                                     "identifier-width=\"11\" request-id=\"0x630\" "
                                     "reply-id=\"0x631\"",
                                     "identifier-width=\"29\" request-id=\"0x1fffffff\" reply-id=\"0x1ffffffe\"");
    auto extended = decode_dashboard_document(byte_view(extended_xml));
    ASSERT_TRUE(extended) << extended.error().detail;
    EXPECT_EQ(extended->connection.identifier_width, CanIdentifierWidth::Extended);
    EXPECT_EQ(extended->connection.request_id, 0x1fffffffu);
}

TEST(DashboardCodec, RejectsNonDecimalUnsignedTextOverflowSignsAndTrailingJunk)
{
    struct NumberCase
    {
        std::string_view original;
        std::string_view replacement;
        std::string_view path;
    };
    constexpr std::array cases{
        NumberCase{"bitrate=\"500000\"", "bitrate=\"0x7a120\"", "connection.bitrate"},
        NumberCase{"bitrate=\"500000\"", "bitrate=\"+500000\"", "connection.bitrate"},
        NumberCase{"bitrate=\"500000\"", "bitrate=\"-1\"", "connection.bitrate"},
        NumberCase{"bitrate=\"500000\"", "bitrate=\"500000ms\"", "connection.bitrate"},
        NumberCase{"bitrate=\"500000\"", "bitrate=\"4294967296\"", "connection.bitrate"},
        NumberCase{"stream-instance=\"0\"", "stream-instance=\"256\"", "connection.stream-instance"},
        NumberCase{"precision=\"0\"", "precision=\"256\"",
                   "channels[CDBG_ENGINE_RPM].conversions[conversion-1].precision"},
        NumberCase{"request-id=\"0x630\"", "request-id=\"630\"", "connection.request-id"},
        NumberCase{"request-id=\"0x630\"", "request-id=\"0X630\"", "connection.request-id"},
        NumberCase{"request-id=\"0x630\"", "request-id=\"0x+630\"", "connection.request-id"},
        NumberCase{"request-id=\"0x630\"", "request-id=\"0x630can\"", "connection.request-id"},
        NumberCase{"request-id=\"0x630\"", "request-id=\"0x100000000\"", "connection.request-id"},
    };

    for (const auto& test_case : cases)
    {
        expect_decode_error(replace_once(std::string(kCanonicalXml), test_case.original, test_case.replacement),
                            ErrorKind::InvalidConfig, test_case.path);
    }
}

TEST(DashboardCodec, RejectsNonfiniteAndMalformedFloatingPointText)
{
    for (const std::string_view invalid : {"nan", "inf", "-inf", "+1", "1.0rpm", ""})
    {
        expect_decode_error(
            replace_once(std::string(kCanonicalXml), "gauge-min=\"0\"", "gauge-min=\"" + std::string(invalid) + "\""),
            ErrorKind::InvalidConfig, "channels[CDBG_ENGINE_RPM].conversions[conversion-1].gauge-min");
    }
}

TEST(DashboardCodec, RejectsInvalidEnumSpellings)
{
    struct EnumCase
    {
        std::string_view original;
        std::string_view replacement;
        std::string_view path;
    };
    constexpr std::array cases{
        EnumCase{"protocol=\"cdbg\"", "protocol=\"CDBG\"", "connection.protocol"},
        EnumCase{"transport=\"raw-can\"", "transport=\"can\"", "connection.transport"},
        EnumCase{"identifier-width=\"11\"", "identifier-width=\"standard\"", "connection.identifier-width"},
        EnumCase{"raw-assembly=\"unsigned-integer-decimal\"", "raw-assembly=\"uint\"",
                 "channels[CDBG_ENGINE_RPM].raw-assembly"},
    };
    for (const auto& test_case : cases)
    {
        expect_decode_error(replace_once(std::string(kCanonicalXml), test_case.original, test_case.replacement),
                            ErrorKind::InvalidConfig, test_case.path);
    }

    expect_decode_error(with_adapter("kind=\"J2534\" vendor=\"OpenECU\" display-name=\"CAN\""),
                        ErrorKind::InvalidConfig, "connection.preferred-adapter.kind");
    expect_decode_error(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                  "display-type=\"gauge\" order=\"0\""),
                        ErrorKind::InvalidConfig, "cards[rpm].display-type");
}

TEST(DashboardCodec, RequiresTheCompleteGaugeTripletAndPreservesAbsentOptionals)
{
    expect_decode_error(with_card("id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                                  "display-type=\"horizontal-gauge\" order=\"0\" gauge-min=\"0\" "
                                  "gauge-max=\"9000\""),
                        ErrorKind::InvalidConfig, "cards[rpm].gauge");

    auto document = test::valid_document();
    document.metadata.description = std::nullopt;
    const auto encoded = encode_dashboard_document(document);
    ASSERT_TRUE(encoded) << encoded.error().detail;
    const std::string text(encoded->begin(), encoded->end());
    EXPECT_NE(text.find("<metadata name=\"Colt Dashboard\"/>"), std::string::npos);
    EXPECT_EQ(text.find("preferred-adapter"), std::string::npos);
    EXPECT_EQ(text.find("<card "), std::string::npos);
}

TEST(DashboardCodec, EncodesCardsByOrderAndUsesCanonicalNumberSpellings)
{
    auto document = test::valid_document();
    auto second_channel = document.channels.front();
    second_channel.id = "CDBG_COOLANT";
    second_channel.name = "Coolant";
    second_channel.description = "coolant uint16";
    second_channel.address = 0x00ABCDEF;
    second_channel.conversions.front().id = "temperature";
    second_channel.conversions.front().gauge_min = -40.5;
    second_channel.conversions.front().gauge_max = 215.25;
    second_channel.conversions.front().gauge_step = 0.125;
    document.channels.push_back(second_channel);
    document.cards = {
        DashboardCard{"coolant", "CDBG_COOLANT", "temperature", CardDisplayType::Sparkline, std::nullopt, 1,
                      std::nullopt, 60},
        DashboardCard{"rpm", "CDBG_ENGINE_RPM", "conversion-1", CardDisplayType::Numeric, std::nullopt, 0, std::nullopt,
                      std::nullopt},
    };

    const auto result = encode_dashboard_document(document);
    ASSERT_TRUE(result) << result.error().detail;
    const std::string text(result->begin(), result->end());
    EXPECT_LT(text.find("id=\"rpm\""), text.find("id=\"coolant\""));
    EXPECT_NE(text.find("address=\"0xabcdef\""), std::string::npos);
    EXPECT_NE(text.find("gauge-min=\"-40.5\" gauge-max=\"215.25\" gauge-step=\"0.125\""), std::string::npos);
    EXPECT_TRUE(text.ends_with('\n'));
    EXPECT_FALSE(text.ends_with("\n\n"));

    const auto decoded = decode_dashboard_document(*result);
    ASSERT_TRUE(decoded) << decoded.error().detail;
    auto canonical_document = document;
    std::ranges::sort(canonical_document.cards, {}, &DashboardCard::order);
    EXPECT_EQ(*decoded, canonical_document);
}

TEST(DashboardCodec, EncodesPresentOptionalAdapterTitleAndGaugeAttributes)
{
    auto document = test::valid_document();
    document.connection.preferred_adapter = PreferredAdapter{AdapterKind::J2534, "M&M", "CAN <USB>"};
    document.cards = {
        DashboardCard{"rpm", "CDBG_ENGINE_RPM", "conversion-1", CardDisplayType::HorizontalGauge, "RPM & boost", 0,
                      GaugeBoundsOverride{-1.5, 9000.25, 0.125}, std::nullopt},
    };

    const auto result = encode_dashboard_document(document);
    ASSERT_TRUE(result) << result.error().detail;
    const std::string text(result->begin(), result->end());
    EXPECT_NE(text.find("<preferred-adapter kind=\"j2534\" vendor=\"M&amp;M\" "
                        "display-name=\"CAN &lt;USB>\"/>"),
              std::string::npos);
    EXPECT_NE(text.find("<card id=\"rpm\" channel-id=\"CDBG_ENGINE_RPM\" conversion-id=\"conversion-1\" "
                        "display-type=\"horizontal-gauge\" title=\"RPM &amp; boost\" order=\"0\" gauge-min=\"-1.5\" "
                        "gauge-max=\"9000.25\" gauge-step=\"0.125\"/>"),
              std::string::npos);

    const auto decoded = decode_dashboard_document(*result);
    ASSERT_TRUE(decoded) << decoded.error().detail;
    EXPECT_EQ(*decoded, document);
}
