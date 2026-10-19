#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace
{

constexpr std::array<std::uint16_t, 4> kKeys{0x1234, 0xABCD, 0x5678, 0xEF01};

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
        benchmark::DoNotOptimize(ssm_protocol::CalculatePayload(bytes::ByteView(payload),
                                                                static_cast<std::uint32_t>(payload.size()), kKeys,
                                                                ssm_protocol::kIndexTransformationStock));
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

} // namespace

BENCHMARK(BM_CalculatePayload)->Arg(16)->Arg(4096)->Arg(1 << 20);
