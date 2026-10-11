#pragma once
// Engine/Desktop/ShellIcons.hpp - the desktop's own shortcuts, read out at runtime.
//
// This is the input side of the icon orbit: it does not draw anything, it answers "what would the
// user click on, and what picture and what program goes with it". Everything here is measured from
// stage 0 (tools/probe-shortcuts.ps1): 43 .lnk files on this machine's Desktop and public Desktop,
// 43 of 43 targets resolving to something that exists, 4 carrying arguments, and
// [System.Drawing.Icon]::ExtractAssociatedIcon returning 32x32 for 8 of 8 - which is why the icon
// comes from the shell instead, at the size we ask for.
//
// Deliberately not included: the shell namespace items (This PC, Recycle Bin, Control Panel). The
// same probe counted 68 items through Shell.Application against 53 on disk, so 15 are namespace-only
// and need a different extraction and a different launch path. The orbit starts with .lnk files.
#include "Engine/Core/Platform.hpp"


namespace sw {

struct ShellIcon {
    std::wstring label;    // the shortcut name as the desktop shows it, without the ".lnk"
    std::wstring lnkPath;  // the .lnk file itself - this is what ShellExecute launches
    std::wstring target;   // what it points at; empty when the shell could not resolve one
    UINT cell = 0;         // index into the atlas grid, row-major
    bool alphaFromMask = false;   // the icon carried no alpha and the AND mask had to be used
};

class ShellIcons {
public:
    // 256 px cells, because that is what the shell's large image list actually holds. Measured first:
    // with a 128 px cell and SHDefExtractIconW the shell returned 32 px for 43 of 43 shortcuts, so
    // the atlas upscaled 4x and the ring's front icons (0.085 * 2160 = 184 device px) upscaled again
    // to 5.75x total - which is the blockiness in his screenshot of the Chrome icon.
    //
    // 8 columns x 6 rows of 256 = 2048x1536 BGRA = 12.6 MB, ~16.7 MB with the mip chain.
    static constexpr UINT kCell = 256;

    // Enumerate both Desktop folders and pull each shortcut's icon and target. Returns false only if
    // nothing at all could be read; individual failures are counted and reported.
    bool Build(std::string& error);

    // Premultiplied BGRA, row-major, top row first. Empty until Build succeeds with at least one icon.
    const std::vector<BYTE>& pixels() const { return atlas_; }
    UINT cols() const { return cols_; }
    UINT rows() const { return rows_; }
    UINT atlasWidth() const { return cols_ * kCell; }
    UINT atlasHeight() const { return rows_ * kCell; }
    const std::vector<ShellIcon>& items() const { return items_; }

    // What the shell reported, for the log line and the self-test.
    UINT countFound() const { return (UINT)items_.size(); }
    UINT countIconsDrawn() const { return drawn_; }
    UINT countMaskOnly() const { return maskOnly_; }
    UINT countUnresolvedTarget() const { return unresolved_; }

private:
    // One HICON into kCell*kCell premultiplied BGRA, copied into `atlas` at grid position `cell`
    // (row-major, `cols` cells wide). Returns false if the icon could not be rendered. Detects the
    // no-alpha case by looking at the alpha the shell actually filled in, and only then falls back to
    // the AND mask.
    bool Blit(HICON icon, UINT cell, UINT cols, std::vector<BYTE>& atlas, bool& viaMask);

    std::vector<ShellIcon> items_;
    std::vector<BYTE> atlas_;
    UINT cols_ = 0, rows_ = 0;
    UINT drawn_ = 0, maskOnly_ = 0, unresolved_ = 0;
    UINT smallest_ = 0, largest_ = 0, belowCell_ = 0;
};

} // namespace sw
