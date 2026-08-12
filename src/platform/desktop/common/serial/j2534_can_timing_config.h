#pragma once

#include <functional>

#if defined(_WIN32)
#include "src/platform/desktop/windows/j2534/J2534_tactrix_win.h"
#else
#include "src/platform/desktop/unix/j2534/J2534_tactrix_unix.h"
#endif

using J2534SetConfig = std::function<long(const SCONFIG_LIST&)>;

bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig);
