#include "Engine/Graphics/Texture.hpp"
#include "Engine/Core/Log.hpp"

#include <algorithm>

namespace sw {
static constexpr const char* MOD = "tex";

bool Texture::Load(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& path, std::string& error,
             bool mipmaps) {
    if (!dev || !wic) {
        error = "no device / WIC factory";
        return false;
    }
    Com<IWICBitmapDecoder> dec;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                &dec);
    if (FAILED(hr)) {
        error = "CreateDecoderFromFilename: " + HResultToString(hr) + " for " + ToUtf8(path);
        return false;
    }
    Com<IWICBitmapFrameDecode> frame;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) {
        error = "GetFrame: " + HResultToString(hr);
        return false;
    }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) {
        error = "zero-sized image " + ToUtf8(path);
        return false;
    }

    Com<IWICFormatConverter> conv;
    hr = wic->CreateFormatConverter(&conv);
    if (FAILED(hr)) {
        error = "CreateFormatConverter: " + HResultToString(hr);
        return false;
    }
    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                          WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        error = "convert to BGRA: " + HResultToString(hr) + " for " + ToUtf8(path);
        return false;
    }

    const UINT stride = w * 4;
    std::vector<BYTE> pixels((size_t)stride * h);
    hr = conv->CopyPixels(nullptr, stride, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) {
        error = "CopyPixels: " + HResultToString(hr);
        return false;
    }

    path_ = path;
    return Upload(dev, w, h, std::move(pixels), error, mipmaps, path);
}

bool Texture::FromPixels(ID3D11Device* dev, UINT w, UINT h, const BYTE* bgra, std::string& error,
                         bool mipmaps) {
    if (!dev) { error = "no device"; return false; }
    if (!w || !h) { error = "zero-sized pixel buffer"; return false; }
    if (!bgra) { error = "null pixel buffer"; return false; }
    path_.clear();
    std::vector<BYTE> pixels(bgra, bgra + (size_t)w * h * 4);
    return Upload(dev, w, h, std::move(pixels), error, mipmaps, L"<memory>");
}

bool Texture::Upload(ID3D11Device* dev, UINT w, UINT h, std::vector<BYTE>&& pixels, std::string& error,
                     bool mipmaps, const std::wstring& label) {
    // A full mip chain, built here on the CPU and handed over as one subresource per level.
    //
    // Why: every wallpaper texture is sampled with D3D11_FILTER_MIN_MAG_MIP_LINEAR
    // (D3D11Renderer.cpp), and with a single level that filter has nothing to choose between -
    // minifying this 2084 px drawing into a 403 px preview box reads one 2x2 footprint per output
    // pixel, so 1 px lines survive only where they happen to land. Measured against a CPU
    // area-averaged reference of the same pixels: 24% of the drawing's line pixels were missing and
    // 32% of what was drawn sat where the drawing has nothing.
    //
    // Why CPU and not ID3D11DeviceContext::GenerateMips: both shapes the GPU offers were rejected
    // with E_INVALIDARG on this device - MipLevels = 0, and an explicit count with
    // D3D11_RESOURCE_MISC_GENERATE_MIPS (with and without pInitialData). The subresource-array path
    // is the one the docs state plainly, and it costs ~10 MB of transient RAM at load.
    UINT levels = 1;
    if (mipmaps)
        for (UINT s = (w > h ? w : h); s > 1; s >>= 1) levels++;

    std::vector<std::vector<BYTE>> chain(levels);
    std::vector<D3D11_SUBRESOURCE_DATA> subs(levels);
    UINT mw = w, mh = h;
    chain[0] = std::move(pixels);
    for (UINT l = 0; l < levels; ++l) {
        subs[l].pSysMem = chain[l].data();
        subs[l].SysMemPitch = mw * 4;
        subs[l].SysMemSlicePitch = 0;
        if (l + 1 == levels) break;
        const UINT nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        chain[l + 1].resize((size_t)nw * nh * 4);
        const BYTE* src = chain[l].data();
        BYTE* dst = chain[l + 1].data();
        // Plain 2x2 box average, clamped at odd edges - the same footprint the hardware would take.
        for (UINT y = 0; y < nh; ++y)
            for (UINT x = 0; x < nw; ++x)
                for (INT c = 0; c < 4; ++c) {
                    UINT sum = 0;
                    for (UINT dy = 0; dy < 2; ++dy)
                        for (UINT dx = 0; dx < 2; ++dx) {
                            const UINT sx = std::min(x * 2 + dx, mw - 1), sy = std::min(y * 2 + dy, mh - 1);
                            sum += src[((size_t)sy * mw + sx) * 4 + c];
                        }
                    dst[((size_t)y * nw + x) * 4 + c] = (BYTE)((sum + 2) / 4);
                }
        mw = nw;
        mh = nh;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, subs.data(), &tex_);
    if (FAILED(hr)) {
        error = "CreateTexture2D: " + HResultToString(hr) + " for " + ToUtf8(label);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = levels;
    hr = dev->CreateShaderResourceView(tex_.Get(), &sv, &srv_);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView: " + HResultToString(hr) + " for " + ToUtf8(label);
        return false;
    }
    w_ = w;
    h_ = h;
    Info(MOD, "loaded {} ({}x{}, {} mips, {} KB)", ToUtf8(label), w, h, levels,
         (unsigned)((size_t)w * h * 4 >> 10));
    return true;
}

bool WriteImage(IWICImagingFactory* wic, const std::wstring& path, UINT w, UINT h, std::vector<BYTE>& bgra,
                std::string& error, int jpegQuality) {
    if (!wic) { error = "no WIC factory"; return false; }
    const bool jpeg = jpegQuality > 0;
    Com<IWICBitmap> bmp;
    HRESULT hr = wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)bgra.size(),
                                             bgra.data(), &bmp);
    if (FAILED(hr)) { error = "CreateBitmapFromMemory: " + HResultToString(hr); return false; }
    Com<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr)) { error = "CreateStream: " + HResultToString(hr); return false; }
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { error = "cannot open " + ToUtf8(path) + ": " + HResultToString(hr); return false; }
    Com<IWICBitmapEncoder> enc;
    hr = wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc);
    if (FAILED(hr)) { error = "CreateEncoder: " + HResultToString(hr); return false; }
    hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { error = "encoder init: " + HResultToString(hr); return false; }
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    hr = jpeg ? enc->CreateNewFrame(&frame, &props) : enc->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) { error = "CreateNewFrame: " + HResultToString(hr); return false; }
    if (jpeg) {
        // WIC's JPEG encoder takes the quality as a named property on the frame's option bag, and it
        // will not accept BGRA - the pixel format below is what makes it drop alpha instead of
        // failing the whole frame.
        PROPBAG2 spec{};
        spec.pstrName = (LPWSTR)L"ImageQuality";
        VARIANT v{};
        v.vt = VT_R4;
        v.fltVal = float(jpegQuality) / 100.f;
        if (FAILED(props->Write(1, &spec, &v))) { error = "jpeg quality property"; return false; }
        VariantClear(&v);
    }
    hr = jpeg ? frame->Initialize(props.Get()) : frame->Initialize(nullptr);
    if (FAILED(hr)) { error = "frame init: " + HResultToString(hr); return false; }
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    hr = frame->WriteSource(bmp.Get(), nullptr);
    if (FAILED(hr)) { error = "WriteSource: " + HResultToString(hr); return false; }
    hr = frame->Commit();
    if (FAILED(hr)) { error = "frame commit: " + HResultToString(hr); return false; }
    hr = enc->Commit();
    if (FAILED(hr)) { error = "encoder commit: " + HResultToString(hr); return false; }
    return true;
}

} // namespace sw
