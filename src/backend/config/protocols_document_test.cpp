#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/config/protocols_document.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <pugixml.hpp>

using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::config::ConfigPaths;
using fastecu::config::load_protocols_document;
using fastecu::config::text_or_empty;

namespace
{
ConfigPaths test_paths()
{
    ConfigPaths p;
    p.protocols_file = "protocols.cfg";
    return p;
}

std::vector<std::uint8_t> as_bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}
} // namespace

TEST(LoadProtocolsDocument, ParsesTheConfiguredProtocolsFile)
{
    InMemoryFileRepository repo;
    repo.files["protocols.cfg"] = as_bytes("<config><protocols><protocol name=\"a\"/></protocols></config>");

    auto doc = load_protocols_document(test_paths(), repo);

    ASSERT_THAT(doc, fastecu::testing::IsOk());
    EXPECT_STREQ(doc->child("config").child("protocols").child("protocol").attribute("name").as_string(), "a");
    EXPECT_THAT(repo.read_handles, ::testing::ElementsAre("protocols.cfg"));
}

TEST(LoadProtocolsDocument, PropagatesTheRepositoryReadError)
{
    InMemoryFileRepository repo;
    repo.read_errors.emplace("protocols.cfg", fastecu::Error{ErrorKind::Internal, "disk gone"});

    EXPECT_THAT(load_protocols_document(test_paths(), repo),
                fastecu::testing::IsErrWith(ErrorKind::Internal, ::testing::HasSubstr("disk gone")));
}

TEST(LoadProtocolsDocument, MalformedXmlIsInvalidConfigNamingTheProtocolsFile)
{
    InMemoryFileRepository repo;
    repo.files["protocols.cfg"] = as_bytes("<config><protocols>");

    EXPECT_THAT(
        load_protocols_document(test_paths(), repo),
        fastecu::testing::IsErrWith(ErrorKind::InvalidConfig, ::testing::StartsWith("protocols parse error: ")));
}

TEST(TextOrEmpty, ReturnsTheNamedChildsTextOrEmptyWhenAbsent)
{
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string("<protocol><ecu>Denso</ecu><mcu/></protocol>"));
    const pugi::xml_node protocol = doc.child("protocol");

    EXPECT_EQ(text_or_empty(protocol, "ecu"), "Denso");
    EXPECT_EQ(text_or_empty(protocol, "mcu"), "");
    EXPECT_EQ(text_or_empty(protocol, "kernel"), "");
}
