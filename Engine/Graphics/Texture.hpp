#pragma once
// Engine/Graphics/Texture.hpp - WIC-decoded image turned into a shader resource.
#include "Engine/Core/Platform.hpp"
#include <wincodec.h>

namespace sw {

class Texture {
public:
    // Decodes through WIC into CPU pixels, then uploads once. Wallpapers are loaded a handful of times,
    // so the staging copy is cheaper than holding a second decoded format around.
    //
    // mipmaps=false leaves the view at level 0, which is what the HUD's glyph atlas wants: its cells
    // are 48 px tall and the card draws them at 37.8 px on this screen, so a chain would blend in 34%
    // of the next level and soften text he has already signed off on.
    bool Load(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& path, std::string& error,
              bool mipmaps = true);

    // The same upload starting from pixels that were never in a file. The desktop icon atlas is built
    // at runtime out of the shell's own icons, so there is no path to hand WIC - and writing the atlas
    // to disk first would put the user's shortcut names and icon art into our cache directory.
    // `bgra` is w*h*4 bytes, row-major, top row first, and is expected to be PREMULTIPLIED because the
    // overlay that reads it composites with premultiplied SRC_ALPHA.
    bool FromPixels(ID3D11Device* dev, UINT w, UINT h, const BYTE* bgra, std::string& error,
                    bool mipmaps = true);

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    UINT width() const { return w_; }
    UINT height() const { return h_; }
    const std::wstring& path() const { return path_; }
    bool loaded() const { return srv_ != nullptr; }

private:
    // The part Load and FromPixels agree on: build a mip chain on the CPU and create the texture and
    // its view. `label` only exists so the log line can name what failed.
    bool Upload(ID3D11Device* dev, UINT w, UINT h, std::vector<BYTE>&& bgra, std::string& error,
                bool mipmaps, const std::wstring& label);

    Com<ID3D11Texture2D> tex_;
    Com<ID3D11ShaderResourceView> srv_;
    UINT w_ = 0, h_ = 0;
    std::wstring path_;
};

// Encodes CPU-side BGRA pixels as a PNG file through WIC.
// jpegQuality > 0 writes a JPEG instead of a PNG; the motion strip is 24 frames of a 1280x720
// picture and lossless would cost ~28 MB per wallpaper.
bool WriteImage(IWICImagingFactory* wic, const std::wstring& path, UINT w, UINT h, std::vector<BYTE>& bgra,
                std::string& error, int jpegQuality = 0);

} // namespace sw
