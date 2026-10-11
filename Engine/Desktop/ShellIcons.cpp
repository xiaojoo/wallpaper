#include "Engine/Desktop/ShellIcons.hpp"
#include "Engine/Core/Log.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>   // the full IImageList definition; shobjidl.h only forward-declares it

#include <algorithm>
#include <cmath>

namespace sw {
static constexpr const char* MOD = "icons";

namespace {

// Both folders the shell unions into the visible desktop.
std::vector<std::wstring> DesktopFolders() {
    std::vector<std::wstring> out;
    const KNOWNFOLDERID ids[] = { FOLDERID_Desktop, FOLDERID_PublicDesktop };
    for (auto id : ids) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
            out.push_back(p);
            CoTaskMemFree(p);
        }
    }
    return out;
}

std::vector<std::wstring> ListShortcuts(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    std::wstring pat = dir + L"\\*.lnk";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring StripExtension(const std::wstring& path) {
    const size_t s = path.find_last_of(L'\\');
    std::wstring name = (s == std::wstring::npos) ? path : path.substr(s + 1);
    const size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos ? name : name.substr(0, d);
}

// What the shortcut points at. Only used for the log and, later, for the label the orbit shows -
// launching goes through the .lnk itself so the shell applies its own elevation and arguments.
std::wstring LinkTarget(const std::wstring& lnk) {
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return {};
    Com<IPersistFile> pf;
    if (FAILED(link.As(&pf))) return {};
    if (FAILED(pf->Load(lnk.c_str(), STGM_READ))) return {};
    WCHAR buf[MAX_PATH]{};
    if (FAILED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_UNCPRIORITY))) return {};
    return buf;
}

} // namespace

bool ShellIcons::Blit(HICON icon, UINT cell, UINT cols, std::vector<BYTE>& atlas, bool& viaMask) {
    viaMask = false;
    if (!icon) return false;
    const UINT C = kCell;
    const int stride = (int)C * 4;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)C;
    bi.bmiHeader.biHeight = -(LONG)C;   // top-down, so the atlas rows read the same way the shader does
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    std::vector<BYTE> px;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        // Zero first: CreateDIBSection leaves the buffer uninitialised, and a garbage alpha would
        // premultiply into visible noise around the icon.
        SecureZeroMemory(bits, (size_t)stride * C);
        ok = DrawIconEx(mem, 0, 0, icon, (int)C, (int)C, 0, nullptr, DI_NORMAL) != FALSE;
        SelectObject(mem, old);
        px.assign((BYTE*)bits, (BYTE*)bits + (size_t)stride * C);
    }
    DeleteDC(mem);
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    if (!ok || px.empty()) return false;

    // What the shell actually handed over, measured rather than assumed: the icon's colour bitmap
    // edge. The atlas cell is kCell, so an edge below kCell means we are upscaling inside the atlas
    // before the ring ever magnifies it again, and an edge above kCell means the cell is throwing
    // pixels away. The ring draws its front icons at 0.085 * 2160 = 184 device px, so a 128 px cell
    // is already a 1.44x enlargement at the front of the ring.
    {
        ICONINFO qi{};
        if (GetIconInfo(icon, &qi)) {
            BITMAP bm{};
            if (qi.hbmColor && GetObjectW(qi.hbmColor, (int)sizeof(bm), &bm)) {
                const int edge = bm.bmHeight > 0 ? bm.bmWidth : bm.bmWidth * 2;   // monochrome: mask only, double height
                smallest_ = smallest_ ? std::min(smallest_, (UINT)edge) : (UINT)edge;
                largest_ = std::max(largest_, (UINT)edge);
                if ((UINT)edge < kCell) belowCell_++;
            }
            if (qi.hbmColor) DeleteObject(qi.hbmColor);
            if (qi.hbmMask) DeleteObject(qi.hbmMask);
        }
    }

    // Did the shell give us real alpha? A 32bpp icon with an alpha channel leaves the corners at 0;
    // an older colour+mask icon fills the whole alpha row with 255 and paints the background black.
    // Measured rather than assumed, because the two need different handling and only one of them
    // looks wrong if you guess.
    bool hasAlpha = false;
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i] < 250) { hasAlpha = true; break; }

    if (!hasAlpha) {
        ICONINFO ii{};
        if (GetIconInfo(icon, &ii)) {
            std::vector<BYTE> mask;
            if (ii.hbmMask) {
                BITMAPINFO mi{};
                mi.bmiHeader.biSize = sizeof(mi.bmiHeader);
                mi.bmiHeader.biWidth = (LONG)C;
                mi.bmiHeader.biHeight = -(LONG)C;
                mi.bmiHeader.biPlanes = 1;
                mi.bmiHeader.biBitCount = 1;
                mi.bmiHeader.biCompression = BI_RGB;
                const int rowBytes = ((int)C + 31) / 32 * 4;
                mask.assign((size_t)rowBytes * C, 0);
                HDC s2 = GetDC(nullptr);
                GetDIBits(s2, ii.hbmMask, 0, C, mask.data(), &mi, DIB_RGB_COLORS);
                ReleaseDC(nullptr, s2);
            }
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            if (!mask.empty()) {
                const int rowBytes = ((int)C + 31) / 32 * 4;
                for (UINT y = 0; y < C; ++y)
                    for (UINT x = 0; x < C; ++x) {
                        const bool transparent = (mask[(size_t)y * rowBytes + x / 8] >> (7 - x % 8)) & 1;
                        if (transparent)
                            for (int c = 0; c < 4; ++c) px[(size_t)y * stride + (size_t)x * 4 + c] = 0;
                        else
                            px[(size_t)y * stride + (size_t)x * 4 + 3] = 255;
                    }
                viaMask = true;
            }
        }
    }

    // Premultiply: the overlay composites with premultiplied SRC_ALPHA (D3D11Renderer's over_ state),
    // and BGRA is already the byte order DXGI_FORMAT_B8G8R8A8_UNORM wants.
    for (size_t i = 0; i < px.size(); i += 4) {
        const UINT a = px[i + 3];
        if (a == 255) continue;
        if (a == 0) { px[i] = px[i + 1] = px[i + 2] = 0; continue; }
        for (int c = 0; c < 3; ++c) px[i + c] = (BYTE)((px[i + c] * a + 127) / 255);
    }

    const UINT atlasStride = cols * C * 4;
    const UINT col = cell % cols, row = cell / cols;
    if (atlas.size() < (size_t)atlasStride * C * (row + 1)) return false;
    // Copy the finished cell with a transparent gutter, by sampling rather than by drawing smaller:
    // the alpha decision above (real alpha vs a colour+mask icon) is made against this buffer's full
    // extent, and insetting here is the one place that cannot disagree with it.
    const UINT inner = C - 2 * kCellPad;
    for (UINT y = 0; y < C; ++y)
        for (UINT x = 0; x < C; ++x) {
            BYTE* d = atlas.data() + (size_t)(row * C + y) * atlasStride + (size_t)(col * C + x) * 4;
            const int sx = (int)x - (int)kCellPad, sy = (int)y - (int)kCellPad;
            if (sx < 0 || sy < 0 || sx >= (int)inner || sy >= (int)inner) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            // Bilinear, on the premultiplied buffer - which is the only space where interpolating the
            // colour channels is legal.
            const float fx = (sx + 0.5f) * C / (float)inner - 0.5f;
            const float fy = (sy + 0.5f) * C / (float)inner - 0.5f;
            const int x0 = (int)std::clamp(std::floor(fx), 0.f, (float)(C - 1));
            const int y0 = (int)std::clamp(std::floor(fy), 0.f, (float)(C - 1));
            const int x1 = std::min(x0 + 1, (int)C - 1), y1 = std::min(y0 + 1, (int)C - 1);
            const float ax = fx - x0, ay = fy - y0;
            for (int c = 0; c < 4; ++c) {
                const float v0 = px[(size_t)y0 * stride + (size_t)x0 * 4 + c] * (1 - ax) +
                                 px[(size_t)y0 * stride + (size_t)x1 * 4 + c] * ax;
                const float v1 = px[(size_t)y1 * stride + (size_t)x0 * 4 + c] * (1 - ax) +
                                 px[(size_t)y1 * stride + (size_t)x1 * 4 + c] * ax;
                d[c] = (BYTE)std::min<float>(255.f, v0 * (1 - ay) + v1 * ay + 0.5f);
            }
        }
    return true;
}

bool ShellIcons::Build(std::string& error) {
    items_.clear();
    atlas_.clear();
    drawn_ = maskOnly_ = unresolved_ = 0;

    std::vector<std::wstring> paths;
    for (auto& dir : DesktopFolders()) {
        for (auto& p : ListShortcuts(dir)) paths.push_back(p);
    }
    // Sorted by the name the desktop shows, not by the full path. The two Desktop folders are unioned
    // into one ring, and a path sort grouped them by folder: every one of C:\Users\Public\Desktop's
    // shortcuts came before a single item on the user's own desktop, purely because "P" precedes "X",
    // so a sixteen-icon ring could only ever offer public programs.
    std::sort(paths.begin(), paths.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return StripExtension(a) < StripExtension(b);
              });
    if (paths.empty()) {
        error = "no .lnk found on either Desktop folder";
        return false;
    }

    const UINT C = kCell;
    cols_ = 8;
    rows_ = (UINT)((paths.size() + cols_ - 1) / cols_);
    if (!rows_) rows_ = 1;
    atlas_.assign((size_t)cols_ * rows_ * C * C * 4, 0);

    UINT cell = 0;
    for (auto& p : paths) {
        HICON big = nullptr, hSmall = nullptr;
        const std::wstring target = LinkTarget(p);
        // Measured, not assumed: SHDefExtractIconW on the .lnk itself gave 32 px for 43 of 43
        // shortcuts on this machine, because index -1 on a link resolves through the shell's small
        // icon. The target's own resource carries the large one, so ask the target at C px first and
        // fall back to the link. That 32 px source is why the Chrome icon in the ring read as a
        // 5.75x blocky enlargement: 32 -> a 128 px cell, then a 184 px drawn size at the front.
        HRESULT hr = target.empty() ? E_FAIL
                                    : SHDefExtractIconW(target.c_str(), 0, 0, &big, &hSmall, C);
        if (FAILED(hr) || !big) {
            big = nullptr;
            hr = SHDefExtractIconW(p.c_str(), -1, 0, &big, &hSmall, C);
        }
        bool got = SUCCEEDED(hr) && big;
        if (!got) {
            // The documented fallback for anything SHDefExtractIcon declines. SHGFI_ICON hands us an
            // icon we own, so it must be destroyed.
            big = nullptr;
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_LARGEICON | SHGFI_ICON)) {
                big = sfi.hIcon;
                got = big != nullptr;
            }
        }
        ShellIcon it;
        it.lnkPath = p;
        it.label = StripExtension(p);
        it.target = target;
        it.cell = cell;
        if (target.empty()) unresolved_++;

        bool viaMask = false;
        if (got && Blit(big, cell, cols_, atlas_, viaMask)) {
            drawn_++;
            if (viaMask) maskOnly_++;
            it.alphaFromMask = viaMask;
            items_.push_back(std::move(it));
            cell++;
        }
        if (big) DestroyIcon(big);
        if (hSmall) DestroyIcon(hSmall);
    }
    rows_ = (UINT)((cell + cols_ - 1) / cols_);
    if (!rows_) rows_ = 1;
    atlas_.resize((size_t)cols_ * rows_ * C * C * 4, 0);

    Info(MOD, "desktop shortcuts: {} found, {} drawn into a {}x{} cell atlas, {} needed the AND mask, {} targets unresolved",
         paths.size(), drawn_, cols_, rows_, maskOnly_, unresolved_);
    Info(MOD, "shell icon edges: smallest {} px, largest {} px, {} of {} below the {} px cell",
         smallest_, largest_, belowCell_, drawn_, kCell);
    if (!drawn_) {
        error = "found " + std::to_string(paths.size()) + " shortcuts but drew none of their icons";
        return false;
    }
    return true;
}

} // namespace sw
