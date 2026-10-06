#pragma once
// Engine/Core/Platform.hpp - the single place Win32/D3D headers come from.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Core/Json.hpp" // ToWide/ToUtf8

namespace sw {

template <class T> using Com = Microsoft::WRL::ComPtr<T>;

struct Rect {
    long x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool valid() const { return w > 0 && h > 0; }
    bool contains(long px, long py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

inline RECT ToRECT(const Rect& r) { return RECT{r.x, r.y, r.x + r.w, r.y + r.h}; }

// Log helper: "0x887A0004 (something)" - HResultToString lives in Log.hpp.
std::string HResultToString(long hr);

} // namespace sw
