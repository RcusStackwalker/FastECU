#pragma once

#include <chrono>
#include <cstddef>
#include <functional>

namespace fastecu::desktop::connection::detail
{

// Package-private seam around the blocking-capable SocketCAN operations.
struct SocketCanActions
{
    std::function<std::ptrdiff_t(int, const void *, std::size_t, int)> send;
    std::function<int(int, short, int, short&)> poll;
    std::function<std::ptrdiff_t(int, void *, std::size_t, int)> recv;
    std::function<int(int)> close;
    std::function<std::chrono::steady_clock::time_point()> now;
};

SocketCanActions production_socketcan_actions();

} // namespace fastecu::desktop::connection::detail
