#pragma once
// Engine/App/PreviewStream.hpp - the one shared-memory section the renderer writes and the settings
// window reads for the live preview. Header only because it is compiled into both processes and the
// layout has to be the same on both sides.
//
// A seqlock rather than a mutex: the writer must never wait for a reader (it is in the frame loop),
// and the reader would rather see the next frame than a half-copied one.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace sw {

constexpr UINT32 kPreviewMagic = 0x56535057;   // 'WPSV'
constexpr UINT32 kPreviewVersion = 2;   // 2: the header says which wallpaper the frame is
constexpr UINT32 kPreviewMaxW = 1280, kPreviewMaxH = 720;
inline constexpr wchar_t kPreviewMapName[] = L"Local\\SmartWallpaper.Preview";
inline constexpr size_t kPreviewHeaderBytes = 64;
inline constexpr size_t kPreviewBytes = kPreviewHeaderBytes + size_t(kPreviewMaxW) * kPreviewMaxH * 4;

struct PreviewHeader {
    UINT32 magic = 0, version = 0;
    UINT32 w = 0, h = 0, stride = 0;   // stride is w * 4, rows top-down, BGRA8
    volatile LONG seq = 0;             // odd while the payload is being written
    UINT64 stampMs = 0;                // GetTickCount64 when the frame left the GPU
    UINT32 frames = 0;
    UINT64 idHash = 0;                 // which wallpaper this frame is; see IdHash below
};

// The reader must not show a frame of the wallpaper the user *left* while the first frame of the
// one they picked is still in flight, so the section says what it holds. Any stable 64-bit digest
// works as long as both sides agree on it.
inline UINT64 IdHash(const char* s) {
    UINT64 h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= static_cast<UINT64>(static_cast<unsigned char>(*s)); h *= 1099511628211ULL; }
    return h;
}
static_assert(sizeof(PreviewHeader) <= kPreviewHeaderBytes, "preview header must fit its reserved block");

class PreviewStream {
public:
    ~PreviewStream() { Close(); }

    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        view_ = nullptr;
        map_ = nullptr;
        hd_ = nullptr;
    }

    // The writer owns the section; recreating it is only needed if the size ceiling changes.
    bool OpenWriter(std::string& error) {
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  DWORD(kPreviewBytes >> 32), DWORD(kPreviewBytes & 0xFFFFFFFF), kPreviewMapName);
        if (!map_) {
            error = std::format("CreateFileMapping: gle {}", GetLastError());
            return false;
        }
        view_ = MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, kPreviewBytes);
        if (!view_) {
            error = std::format("MapViewOfFile: gle {}", GetLastError());
            Close();
            return false;
        }
        hd_ = static_cast<PreviewHeader*>(view_);
        hd_->magic = kPreviewMagic;
        hd_->version = kPreviewVersion;
        hd_->frames = 0;
        hd_->seq = 0;
        return true;
    }

    void Write(UINT w, UINT h, const BYTE* pixels, UINT srcPitch, ULONGLONG stampMs, UINT64 idHash) {
        if (!hd_ || w == 0 || h == 0 || w > kPreviewMaxW || h > kPreviewMaxH) return;
        InterlockedIncrement(&hd_->seq);
        MemoryBarrier();
        hd_->w = w;
        hd_->h = h;
        hd_->stride = w * 4;
        BYTE* dst = Payload();
        for (UINT y = 0; y < h; ++y)
            memcpy(dst + size_t(y) * w * 4, pixels + size_t(y) * srcPitch, size_t(w) * 4);
        hd_->stampMs = stampMs;
        hd_->idHash = idHash;
        ++hd_->frames;
        MemoryBarrier();
        InterlockedIncrement(&hd_->seq);
    }

    bool open() const { return view_ != nullptr; }
    UINT32 frames() const { return hd_ ? hd_->frames : 0; }

private:
    BYTE* Payload() { return static_cast<BYTE*>(view_) + kPreviewHeaderBytes; }

    HANDLE map_ = nullptr;
    void* view_ = nullptr;
    PreviewHeader* hd_ = nullptr;
};

// The reader side, in the settings window. dst must hold kPreviewBytes of room.
class PreviewReader {
public:
    ~PreviewReader() { Close(); }

    bool Open(std::string& error) {
        map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kPreviewMapName);
        if (!map_) {
            error = std::format("OpenFileMapping: gle {}", GetLastError());
            return false;
        }
        view_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, kPreviewBytes);
        if (!view_) {
            error = std::format("MapViewOfFile: gle {}", GetLastError());
            return false;
        }
        const auto* hd = static_cast<const PreviewHeader*>(view_);
        if (hd->magic != kPreviewMagic || hd->version != kPreviewVersion) {
            error = "preview section layout mismatch - the renderer and the settings window are different builds";
            return false;
        }
        return true;
    }

    // False when nothing new has arrived, or when the copy was caught mid-write (both leave the
    // caller with the previous frame rather than a torn one).
    bool Newest(BYTE* dst, size_t dstCapacity, UINT& w, UINT& h, UINT32& frames, ULONGLONG& stampMs,
                UINT64* idHash = nullptr) {
        if (!view_) return false;
        const auto* hd = static_cast<const PreviewHeader*>(view_);
        const BYTE* src = static_cast<const BYTE*>(view_) + kPreviewHeaderBytes;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = hd->seq;
            if (before & 1) continue;                       // writer is inside the payload
            const UINT fw = hd->w, fh = hd->h, fstride = hd->stride;
            const size_t bytes = size_t(fh) * fstride;   // rows times a row; stride already is w * 4
            if (fw == 0 || fh == 0 || bytes > dstCapacity) return false;
            memcpy(dst, src, bytes);
            MemoryBarrier();
            if (hd->seq != before) continue;                // overwritten during the copy
            w = fw;
            h = fh;
            frames = hd->frames;
            stampMs = hd->stampMs;
            if (idHash) *idHash = hd->idHash;
            return true;
        }
        return false;
    }

    // Header-only peeks, for a poll that must not copy 2 MB just to ask whether there is news.
    // Read without the seqlock: a torn read here costs one skipped frame, not a wrong picture.
    UINT32 frames() const { return view_ ? hdr()->frames : 0; }
    UINT32 width() const { return view_ ? hdr()->w : 0; }
    UINT32 height() const { return view_ ? hdr()->h : 0; }
    ULONGLONG stampMs() const { return view_ ? hdr()->stampMs : 0; }

    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        view_ = nullptr;
        map_ = nullptr;
    }

    bool open() const { return view_ != nullptr; }

private:
    const PreviewHeader* hdr() const { return static_cast<const PreviewHeader*>(view_); }

    HANDLE map_ = nullptr;
    void* view_ = nullptr;
};

} // namespace sw
