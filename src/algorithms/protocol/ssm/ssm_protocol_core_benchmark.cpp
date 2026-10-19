#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace
{

constexpr std::array<std::uint16_t, 4> kKeys{0x1234, 0xABCD, 0x5678, 0xEF01};

constexpr std::array<std::uint16_t, 16> kSeedKeys{0x1234, 0xABCD, 0x5678, 0xEF01, 0x0F0F, 0x9A9A, 0x3C3C, 0xC3C3,
                                                  0x7777, 0x1357, 0x2468, 0xDEAD, 0xBEEF, 0x4242, 0x0001, 0xFFFE};

bytes::Bytes MakePayload(std::size_t size)
{
    bytes::Bytes payload(size);
    std::uint32_t state = 0x9E3779B9U;
    for (bytes::Byte& byte : payload)
    {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<bytes::Byte>(state >> 24U);
    }
    return payload;
}

void BM_CalculatePayload(benchmark::State& state)
{
    const bytes::Bytes payload = MakePayload(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(
            ssm_protocol::CalculatePayload(bytes::ByteView(payload), kKeys, ssm_protocol::kIndexTransformationStock));
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

void BM_CalculateSeedKey(benchmark::State& state)
{
    const bytes::Bytes seed = MakePayload(4);
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(
            ssm_protocol::CalculateSeedKey(bytes::ByteView(seed), kSeedKeys, ssm_protocol::kIndexTransformationStock));
    }
}

} // namespace

BENCHMARK(BM_CalculateSeedKey);
BENCHMARK(BM_CalculatePayload)->Arg(16)->Arg(4096)->Arg(1 << 20);
