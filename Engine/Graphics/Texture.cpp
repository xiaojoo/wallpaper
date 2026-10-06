#include "Engine/Graphics/Texture.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "tex";

bool Texture::Load(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& path, std::string& error) {
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

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = stride;
    hr = dev->CreateTexture2D(&td, &sd, &tex_);
    if (FAILED(hr)) {
        error = "CreateTexture2D: " + HResultToString(hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    hr = dev->CreateShaderResourceView(tex_.Get(), &sv, &srv_);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView: " + HResultToString(hr);
        return false;
    }
    w_ = w;
    h_ = h;
    path_ = path;
    Info(MOD, "loaded {} ({}x{}, {} KB)", ToUtf8(path), w, h, (unsigned)(pixels.size() >> 10));
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
