#pragma once

// The platform J2534 API behind one include path,
// src/platform/desktop/j2534/j2534_api.h. The consumer's BUILD select()
// decides whether that path resolves to this header or the Windows one.
#include "src/platform/desktop/windows/j2534/J2534_win.h"
