#include "src/algorithms/protocol/biu/subaru_biu_frame.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::ElementsAre;

namespace
{

TEST(SubaruBiuFrame, KeepAliveIsTheTesterPresentFrame)
{
    EXPECT_THAT(biu_subaru::keepAliveRequest(), ElementsAre(0x81, 0x40, 0xF0, 0x3E, 0xEF));
}

TEST(SubaruBiuFrame, ConnectRequestCarriesLengthAndChecksum)
{
    const bytes::Bytes cmd{0x81};
    EXPECT_THAT(biu_subaru::buildRequest(cmd), ElementsAre(0x81, 0x40, 0xF0, 0x81, 0x32));
}

TEST(SubaruBiuFrame, MultiByteRequestFoldsPayloadLengthIntoFormatByte)
{
    const bytes::Bytes cmd{0x21, 0x50};
    EXPECT_THAT(biu_subaru::buildRequest(cmd), ElementsAre(0x82, 0x40, 0xF0, 0x21, 0x50, 0x23));
}

TEST(SubaruBiuFrame, RequestsValidateAgainstTheirOwnChecksum)
{
    const bytes::Bytes cmd{0x3E, 0x8A, 0x01, 0x02, 0x03};
    const bytes::Bytes frame = biu_subaru::buildRequest(cmd);
    EXPECT_EQ(bytes::sum8(bytes::ByteView(frame).first(frame.size() - 1)), frame.back());
}

} // namespace
