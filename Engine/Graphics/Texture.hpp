#pragma once
// Engine/Graphics/Texture.hpp - WIC-decoded image turned into a shader resource.
#include "Engine/Core/Platform.hpp"
#include <wincodec.h>

namespace sw {

class Texture {
public:
    // Decodes through WIC into CPU pixels, then uploads once. Wallpapers are loaded a handful of times,
    // so the staging copy is cheaper than holding a second decoded format around.
    bool Load(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& path, std::string& error);

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    UINT width() const { return w_; }
    UINT height() const { return h_; }
    const std::wstring& path() const { return path_; }
    bool loaded() const { return srv_ != nullptr; }

private:
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
