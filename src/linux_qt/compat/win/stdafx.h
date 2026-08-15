#pragma once

// MinGW replacement for Hammer's MFC precompiled header (src/hammer/stdafx.h).
// The licensed sources branch on _WIN32 and include <stdafx.h>, whose real
// Windows form drags in MFC (<afxwin.h>) — which MinGW does not have. The
// _WIN32 code paths themselves only need the Win32 API, so this shim supplies
// <windows.h> plus the same standard-library set the Linux replacement
// header (src/hammer/linux_pch.h) provides.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
