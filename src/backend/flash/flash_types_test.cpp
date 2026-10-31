// src/backend/flash/flash_types_test.cpp
#include "src/backend/flash/flash_types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

namespace fastecu::flash
{
namespace
{

TEST(FlashTypesTest, FamilyPlanHoldsKlineVariant)
{
    FamilyPlan plan = DensoSh705xEepromKlinePlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .kernel_baud = 15625,
    };

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromKlinePlan>(plan));
    EXPECT_EQ(std::get<DensoSh705xEepromKlinePlan>(plan).tester_id, 0xf0);
}

TEST(FlashTypesTest, FamilyPlanHoldsCanVariant)
{
    FamilyPlan plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode3,
        .security = DensoSecurityVariant::kEcuTek,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromCanPlan>(plan));
    EXPECT_EQ(std::get<DensoSh705xEepromCanPlan>(plan).response_id, 0x7e8U);
}

TEST(FlashTypesTest, MixedCanTransportKindIsDistinctFromIso15765)
{
    EXPECT_NE(TransportKind::kCanRawIso15765, TransportKind::kCanIso15765);
}

TEST(FlashTypesTest, FamilyPlanHoldsMitsuColtM32rCanVariant)
{
    FamilyPlan plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = true,
        .session_id = 0x85,
    };

    ASSERT_TRUE(std::holds_alternative<MitsuColtM32rCanPlan>(plan));
    const auto *colt_plan = std::get_if<MitsuColtM32rCanPlan>(&plan);
    ASSERT_NE(colt_plan, nullptr);
    EXPECT_EQ(colt_plan->request_id, 0x7e0U);
    EXPECT_EQ(colt_plan->response_id, 0x7e8U);
    EXPECT_EQ(colt_plan->bitrate, 500000);
    EXPECT_FALSE(colt_plan->extended_id);
    EXPECT_TRUE(colt_plan->use_vendor_challenge);
    EXPECT_EQ(colt_plan->session_id, 0x85);
}

TEST(FlashTypesTest, KernelImageOwnsItsBytesIndependently)
{
    bytes::Bytes source{0x01, 0x02, 0x03};
    KernelImage kernel{.id = "sh705x-stock", .load_address = 0xffff2000, .bytes = source};

    source[0] = 0xff;

    EXPECT_EQ(kernel.bytes[0], 0x01);
}

TEST(FlashTypesTest, EepromReadModeValuesMatchProtocolBytes)
{
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode2), 0x02);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode3), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode4), 0x04);
}

static_assert(std::variant_size_v<FamilyPlan> == 30,
              "A new FamilyPlan alternative: decide whether it uploads a kernel (opt in with kFamilyRequiresKernel in "
              "its <f>_types.h, and list it in KernelPlanTypes), then bump this count.");

using KernelPlanTypes =
    std::tuple<DensoSh705xEepromKlinePlan, DensoSh705xEepromCanPlan, SubaruDensoMc68hc16y5_02Plan,
               SubaruDensoSh7055_02Plan, SubaruDensoSh7058CanPlan, SubaruDensoSh7058CanDieselPlan,
               SubaruDensoSh705xDensoCanPlan, SubaruDensoSh705xKlinePlan, SubaruTcuDensoSh705xCanPlan>;

template <typename T, typename... Ts> constexpr bool kIsOneOf = (std::is_same_v<T, Ts> || ...);
template <typename T, typename Tuple> struct InTuple;
template <typename T, typename... Ts> struct InTuple<T, std::tuple<Ts...>> : std::bool_constant<kIsOneOf<T, Ts...>>
{
};

template <typename Tuple, typename Variant> struct AllInVariant;
template <typename... Ts, typename... Vs>
struct AllInVariant<std::tuple<Ts...>, std::variant<Vs...>> : std::bool_constant<(kIsOneOf<Ts, Vs...> && ...)>
{
};
static_assert(AllInVariant<KernelPlanTypes, FamilyPlan>::value,
              "KernelPlanTypes names a type that is not a FamilyPlan alternative");

template <typename T> void ExpectKernelRequirementMatchesList(std::size_t index)
{
    SCOPED_TRACE("FamilyPlan alternative " + std::to_string(index) + " (" +
                 std::string(FlashFamilyName(FamilyTraits<T>::kFamily)) + ")");
    EXPECT_EQ(kFamilyRequiresKernel<T>, (InTuple<T, KernelPlanTypes>::value));
}

template <typename... Ts> void ExpectKernelRequirements(std::variant<Ts...> *)
{
    std::size_t index = 0;
    (ExpectKernelRequirementMatchesList<Ts>(index++), ...);
}

TEST(FlashTypesTest, exactlyTheKernelUploadingPlanTypesRequireAKernel)
{
    ExpectKernelRequirements(static_cast<FamilyPlan *>(nullptr));
}

TEST(FlashTypes, DensoMc68AndSh7055FamilyPlansConstructAndHoldValues)
{
    SubaruDensoMc68hc16y5_02Plan mc68{
        .connect_baud = 9600,
        .kernel_baud = 11700,
        .encryption_xor = 0x51,
        .kernel_magic = 0x3940,
        .bootloader_ok = {0x4C, 0x00, 0xB4},
    };
    FamilyPlan mc68_variant = mc68;
    EXPECT_EQ(std::get<SubaruDensoMc68hc16y5_02Plan>(mc68_variant).kernel_baud, 11700);

    SubaruDensoSh7055_02Plan sh7055{.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = true};
    FamilyPlan sh7055_variant = sh7055;
    EXPECT_TRUE(std::get<SubaruDensoSh7055_02Plan>(sh7055_variant).read_ecu_id);

    EXPECT_TRUE(kFamilyRequiresKernel<SubaruDensoMc68hc16y5_02Plan>);
    EXPECT_TRUE(kFamilyRequiresKernel<SubaruDensoSh7055_02Plan>);
}

TEST(FlashTypes, BootloaderDialectFamilyPlansAreKernelFree)
{
    SubaruDenso1n83m_1_5mCanPlan denso{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x10000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan denso_variant = denso;
    EXPECT_EQ(std::get<SubaruDenso1n83m_1_5mCanPlan>(denso_variant).lead_pad_len, 0x10000U);

    EXPECT_FALSE(kFamilyRequiresKernel<SubaruDenso1n83m_1_5mCanPlan>);

    SubaruDensoSh72531CanPlan sh72531{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x8000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan sh72531_variant = sh72531;
    EXPECT_EQ(std::get<SubaruDensoSh72531CanPlan>(sh72531_variant).lead_pad_len, 0x8000U);

    EXPECT_FALSE(kFamilyRequiresKernel<SubaruDensoSh72531CanPlan>);

    SubaruDensoSh72543CanDieselPlan sh72543{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x8000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan sh72543_variant = sh72543;
    EXPECT_EQ(std::get<SubaruDensoSh72543CanDieselPlan>(sh72543_variant).lead_pad_len, 0x8000U);

    EXPECT_FALSE(kFamilyRequiresKernel<SubaruDensoSh72543CanDieselPlan>);

    SubaruDenso1n83m_4mCanPlan denso4m{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x10000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan denso4m_variant = denso4m;
    EXPECT_EQ(std::get<SubaruDenso1n83m_4mCanPlan>(denso4m_variant).lead_pad_len, 0x10000U);

    EXPECT_FALSE(kFamilyRequiresKernel<SubaruDenso1n83m_4mCanPlan>);
}

} // namespace
} // namespace fastecu::flash
