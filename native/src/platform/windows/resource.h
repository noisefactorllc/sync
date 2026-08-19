#pragma once

// Shared by packaging/windows/version.rc.in and app_main.cpp so the icon is
// named in one place. The icon resource only exists when the build machine had
// ImageMagick available to rasterise packaging/Sync.svg (see CMakeLists.txt);
// app_main.cpp falls back to a stock icon when LoadIconW cannot find it, so a
// developer build without ImageMagick still runs.
#define IDI_SYNC_APP 101
