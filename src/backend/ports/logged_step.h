#pragma once

#include <cstddef>
#include <tuple>
#include <utility>

#include "src/backend/ports/event_sink.h"

namespace fastecu
{

// A Status/Result step for and_then chains that logs `parts...` (all but the
// last) at info level, then returns the last part, a step callable taking no
// arguments. The previous step's value, if any, is ignored:
//
//   .and_then(LogThen(events, "Seed key ok", "Jumping to kernel", [&] { return Jump(); }))
//
// Parts are captured by value, so a std::format temporary is safe.
template <typename... Parts> auto LogThen(IEventSink& events, Parts... parts)
{
    static_assert(sizeof...(Parts) >= 2, "LogThen takes at least one message and a step");
    return [&events, parts...](const auto&...)
    {
        const auto all = std::tie(parts...);
        constexpr std::size_t kStep = sizeof...(Parts) - 1;
        [&]<std::size_t... I>(std::index_sequence<I...>)
        { (events.Log(LogLevel::kInfo, std::get<I>(all)), ...); }(std::make_index_sequence<kStep>{});
        return std::get<kStep>(all)();
    };
}

// The infallible tail of a chain: logs `messages...` at info level and drops
// the value, for `.transform(LogDone(events, "Jump to kernel ok"))`.
template <typename... Messages> auto LogDone(IEventSink& events, Messages... messages)
{
    return [&events, messages...](const auto&...) { (events.Log(LogLevel::kInfo, messages), ...); };
}

} // namespace fastecu
