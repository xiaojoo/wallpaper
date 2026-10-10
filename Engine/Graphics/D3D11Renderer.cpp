#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Core/Json.hpp" // ReadFileUtf8
#include <algorithm>

namespace sw {
static constexpr const char* MOD = "rend";

// ---------------------------------------------------------------- RenderSurface

bool RenderSurface::Init(ID3D11Device* dev, IDCompositionDevice* comp, HWND hwnd, UINT w, UINT h,
                         std::string& error) {
    dev_ = dev;
    comp_ = comp;
    HRESULT hr = comp->CreateTargetForHwnd(hwnd, TRUE, &target_);
    if (FAILED(hr)) {
        error = "CreateTargetForHwnd: " + HResultToString(hr);
        return false;
    }
    hr = comp->CreateVisual(&visual_);
    if (FAILED(hr)) {
        error = "CreateVisual: " + HResultToString(hr);
        return false;
    }
    hr = target_->SetRoot(visual_.Get());
    if (FAILED(hr)) {
        error = "SetVisual: " + HResultToString(hr);
        return false;
    }
    if (!Resize(w, h, error)) return false;
    Info(MOD, "composition visual tree ready: {} ({}x{})", ToUtf8(label_), w, h);
    return true;
}

bool RenderSurface::Resize(UINT w, UINT h, std::string& error) {
    if (!dev_ || !comp_) { error = "surface not initialised"; return false; }
    if (w == 0 || h == 0) { error = "zero-sized surface"; return false; }
    if (surface_ && w == w_ && h == h_) return true;

    Com<IDCompositionSurface> fresh;
    HRESULT hr = comp_->CreateSurface(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &fresh);
    if (FAILED(hr)) {
        error = "CreateSurface: " + HResultToString(hr);
        return false;
    }
    rtv_.Reset();
    drawn_.Reset();
    if (visual_ && surface_) visual_->SetContent(nullptr);
    surface_ = fresh;
    w_ = w;
    h_ = h;
    if (visual_) {
        hr = visual_->SetContent(surface_.Get());
        if (FAILED(hr)) {
            error = "SetContent: " + HResultToString(hr);
            return false;
        }
    }
    Info(MOD, "{} surface resized to {}x{}", ToUtf8(label_), w, h);
    return true;
}

void RenderSurface::Release() {
    rtv_.Reset();
    drawn_.Reset();
    readback_.Reset();
    if (visual_) visual_->SetContent(nullptr);
    surface_.Reset();
    visual_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_.Reset();
}

bool RenderSurface::Begin(std::string& error) {
    if (!surface_) { error = "no surface"; return false; }
    void* data = nullptr;
    POINT offset{};
    HRESULT hr = surface_->BeginDraw(nullptr, __uuidof(ID3D11Texture2D), &data, &offset);
    if (FAILED(hr)) {
        error = "BeginDraw: " + HResultToString(hr);
        return false;
    }
    drawn_ = static_cast<ID3D11Texture2D*>(data);
    if (!drawn_) { error = "BeginDraw returned null texture"; return false; }
    if (offset.x || offset.y) {
        error = "unexpected partial update offset";
        drawn_.Reset();
        surface_->EndDraw();
        return false;
    }
    hr = dev_->CreateRenderTargetView(drawn_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        error = "RTV on surface texture: " + HResultToString(hr);
        drawn_.Reset();
        surface_->EndDraw();
        return false;
    }
    return true;
}

bool RenderSurface::SampleCorner(ID3D11DeviceContext* ctx, unsigned long long& checksum, UINT side,
                                UINT corner) {
    checksum = 0;
    if (!drawn_ || !dev_) return false;
    D3D11_TEXTURE2D_DESC gd{};
    drawn_->GetDesc(&gd);
    side = std::min(side, std::min(gd.Width, gd.Height));
    if (!readback_) {
        D3D11_TEXTURE2D_DESC sd = gd;
        sd.Width = side;
        sd.Height = side;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.MiscFlags = 0;
        sd.SampleDesc.Count = 1;
        if (FAILED(dev_->CreateTexture2D(&sd, nullptr, &readback_))) return false;
    }
    D3D11_BOX box{};
    const UINT right = (corner & 1) ? gd.Width - side : side;
    const UINT bottom = (corner & 2) ? gd.Height - side : side;
    box.left = (corner & 1) ? gd.Width - side : 0;
    box.top = (corner & 2) ? gd.Height - side : 0;
    box.front = 0;
    box.back = 1;
    box.right = right;
    box.bottom = bottom;
    ctx->CopySubresourceRegion(readback_.Get(), 0, 0, 0, 0, drawn_.Get(), 0, &box);
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mp))) return false;
    const BYTE* data = static_cast<const BYTE*>(mp.pData);
    unsigned long long h = 1469598103934665603ULL; // FNV-1a over the corner bytes
    for (UINT y = 0; y < side; ++y)
        for (UINT x = 0; x < side * 4; ++x) {
            h ^= data[(size_t)y * mp.RowPitch + x];
            h *= 1099511628211ULL;
        }
    ctx->Unmap(readback_.Get(), 0);
    checksum = h;
    return true;
}

void RenderSurface::End(ID3D11DeviceContext* ctx) {
    if (ctx) ctx->Flush(); // the surface is unlocked by EndDraw, so submission must happen first
    if (surface_) surface_->EndDraw();
    rtv_.Reset();
    drawn_.Reset();
}

// ---------------------------------------------------------------- D3D11Renderer

bool D3D11Renderer::Init(ID3D11Device* dev, std::string& error) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &frameCb_);
    if (FAILED(hr)) { error = "frame cbuffer: " + HResultToString(hr); return false; }

    // b2 is fixed-size, so it is made here once rather than resized per package like paramCb_ is. It
    // is uploaded and bound for every wallpaper, including the ones that never read it - an unused
    // cbuffer is stripped by fxc and has no slot, so those draws bind a buffer nothing samples.
    D3D11_BUFFER_DESC sbd{};
    sbd.ByteWidth = sizeof(StatsCB);
    sbd.Usage = D3D11_USAGE_DEFAULT;
    sbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = dev->CreateBuffer(&sbd, nullptr, &statsCb_);
    if (FAILED(hr)) { error = "stats cbuffer: " + HResultToString(hr); return false; }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = dev->CreateRasterizerState(&rd, &raster_);
    if (FAILED(hr)) { error = "rasterizer: " + HResultToString(hr); return false; }

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    hr = dev->CreateDepthStencilState(&dd, &noDepth_);
    if (FAILED(hr)) { error = "depth state: " + HResultToString(hr); return false; }

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = dev->CreateBlendState(&bld, &opaque_);
    if (FAILED(hr)) { error = "blend opaque: " + HResultToString(hr); return false; }

    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr = dev->CreateBlendState(&bld, &additive_);
    if (FAILED(hr)) { error = "blend additive: " + HResultToString(hr); return false; }

    // Premultiplied over. Neither existing state can draw an opaque panel: additive_ is ONE/ONE,
    // so the card's dark fill would only ever lighten whatever it sat on and its ink would glow.
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr = dev->CreateBlendState(&bld, &over_);
    if (FAILED(hr)) { error = "blend over: " + HResultToString(hr); return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev->CreateSamplerState(&sd, &wrap_);
    if (FAILED(hr)) { error = "sampler wrap: " + HResultToString(hr); return false; }
    sd.AddressU = sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = dev->CreateSamplerState(&sd, &clamp_);
    if (FAILED(hr)) { error = "sampler clamp: " + HResultToString(hr); return false; }

    Info(MOD, "renderer state objects ready");
    return true;
}

// The constants every pass shares: b0 is the engine's frame block, b1 the wallpaper's Params.
bool D3D11Renderer::UploadConstants(ID3D11DeviceContext* ctx, const DrawArgs& args, ID3D11Buffer** paramView,
                                     std::string& error) {
    ctx->UpdateSubresource(frameCb_.Get(), 0, nullptr, &args.frame, 0, 0);
    ctx->UpdateSubresource(statsCb_.Get(), 0, nullptr, &args.stats, 0, 0);
    *paramView = nullptr;
    if (!args.paramBytes) return true;
    if (args.paramBytes != paramCbBytes_) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (args.paramBytes + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Com<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        HRESULT hr = dev->CreateBuffer(&bd, nullptr, &paramCb_);
        if (FAILED(hr)) { error = "param cbuffer: " + HResultToString(hr); return false; }
        paramCbBytes_ = args.paramBytes;
        Info(MOD, "parameter constant buffer sized {} B", bd.ByteWidth);
    }
    ctx->UpdateSubresource(paramCb_.Get(), 0, nullptr, args.params, 0, 0);
    *paramView = paramCb_.Get();
    return true;
}

bool D3D11Renderer::Pass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                         const DrawArgs& args, ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                         ID3D11ShaderResourceView* const* srvs, UINT srvCount, UINT vertexCount,
                         std::string& error) {
    ID3D11Buffer* params = nullptr;
    if (!UploadConstants(ctx, args, &params, error)) return false;

    const float blend[4] = {0, 0, 0, 0};
    D3D11_VIEWPORT vp{0.f, 0.f, (float)w, (float)h, 0.f, 1.f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetState(raster_.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetDepthStencilState(noDepth_.Get(), 0);
    Com<ID3D11BlendState> bs = args.blend == DrawArgs::Blend::Over ? over_
                                 : args.blend == DrawArgs::Blend::Additive ? additive_ : opaque_;
    ctx->OMSetBlendState(bs.Get(), blend, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    if (params) ctx->VSSetConstantBuffers(1, 1, &params);
    ctx->VSSetConstantBuffers(2, 1, statsCb_.GetAddressOf());
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    if (params) ctx->PSSetConstantBuffers(1, 1, &params);
    ctx->PSSetConstantBuffers(2, 1, statsCb_.GetAddressOf());
    if (srvCount) {
        ctx->VSSetShaderResources(0, srvCount, srvs);
        ctx->PSSetShaderResources(0, srvCount, srvs);
        ID3D11SamplerState* samplers[8] = {};
        for (UINT i = 0; i < srvCount && i < 8; ++i) samplers[i] = wrap_.Get();
        ctx->PSSetSamplers(0, srvCount, samplers);
    }
    ctx->Draw(vertexCount, 0);
    return true;
}

bool D3D11Renderer::Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                         const DrawArgs& args, std::string& error) {
    if (!rtv || !args.vs || !args.ps) { error = "missing rtv or shader"; return false; }
    return Pass(ctx, rtv, w, h, args, args.vs, args.ps, args.srvs, args.srvCount, 3, error);
}

bool D3D11Renderer::Dispatch(ID3D11DeviceContext* ctx, const DrawArgs& args, std::string& error) {
    if (!args.cs || !args.uav || !args.particles) { error = "particle dispatch missing cs, uav or srv"; return false; }
    ID3D11Buffer* params = nullptr;
    if (!UploadConstants(ctx, args, &params, error)) return false;

    // A resource cannot stay bound for reading while the compute pass writes it, in either direction.
    ID3D11ShaderResourceView* none[8] = {};
    ctx->VSSetShaderResources(0, 8, none);
    ctx->PSSetShaderResources(0, 8, none);

    ctx->CSSetShader(args.cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    if (params) ctx->CSSetConstantBuffers(1, 1, &params);
    ctx->CSSetConstantBuffers(2, 1, statsCb_.GetAddressOf());
    ID3D11UnorderedAccessView* uav = args.uav;
    const UINT append = (UINT)-1;
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &append);
    ctx->Dispatch((args.particleCount + args.threadsPerGroup - 1) / args.threadsPerGroup, 1, 1);
    uav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    return true;
}

bool D3D11Renderer::DrawParticles(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                                   const DrawArgs& args, std::string& error) {
    if (!rtv || !args.particleVs || !args.ps || !args.particles) {
        error = "particle pass missing rtv, vertex shader, pixel shader or buffer";
        return false;
    }
    ID3D11ShaderResourceView* tab[8];
    UINT n = 0;
    tab[n++] = args.particles; // t0 is the particle buffer here, so textures start at t1
    for (UINT i = 0; i < args.srvCount && n < 8; ++i) tab[n++] = args.srvs[i];

    DrawArgs over = args;
    over.blend = DrawArgs::Blend::Additive; // flakes composite over the background, whatever the manifest asked for
    return Pass(ctx, rtv, w, h, over, args.particleVs, args.ps, tab, n,
                args.particleCount * args.verticesPerParticle, error);
}

bool D3D11Renderer::Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                            const DrawArgs& args, std::string& error) {
    if (!args.particleCount) return Draw(ctx, rtv, w, h, args, error);
    if (!Dispatch(ctx, args, error)) return false;
    if (args.backgroundPs) {
        DrawArgs bg = args;
        bg.particleCount = 0;
        bg.blend = DrawArgs::Blend::Opaque; // the sky is what the flakes are composited onto
        bg.ps = args.backgroundPs;
        if (!Draw(ctx, rtv, w, h, bg, error)) return false;
    }
    return DrawParticles(ctx, rtv, w, h, args, error);
}


// ---------------------------------------------------------------- the overlay

// Compiled once for the process, not per slot: the card has to survive every apply, reload and
// monitor change, and it is the same source and the same atlas whoever is underneath it.
bool D3D11Renderer::InitHud(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& root,
                            std::string& error) {
    hudReady_ = false;
    const std::wstring shaders = root + L"\\Shaders";
    const std::wstring srcPath = shaders + L"\\HudCard.hlsl";
    bool ok = false;
    const std::string src = ReadFileUtf8(srcPath, ok);
    // The path in the message, not just "not readable": this is the one place the engine reaches
    // outside its own directory for a shader, and a staged tree that is missing a file is a
    // different diagnosis from a path built wrong here.
    if (!ok) { error = ToUtf8(srcPath) + " not readable"; return false; }

    Texture atlas;
    if (!atlas.Load(dev, wic, shaders + L"\\glyphs.png", error)) {
        error = "hud glyph atlas: " + error;
        return false;
    }
    glyphs_ = atlas.srv();

    Shader::Entries e{};   // vs_5_0 + ps_5_0 only; the overlay has no compute and no particles
    if (!hud_.Compile(src, L"HudCard.hlsl", { shaders }, e, error)) {
        error = "hud shader: " + error;
        return false;
    }
    if (!hud_.CreatePipeline(dev, error)) { error = "hud pipeline: " + error; return false; }
    // Reflected and then refused, rather than never reflected: b1 is one shared upload slot sized by
    // whichever pass last asked, so an overlay that grew a Params block would fight the wallpaper
    // for it every frame. Reflecting is what makes the guard below able to say anything at all.
    if (!hud_.ReflectParams(error)) { error = "hud params: " + error; return false; }
    if (hud_.hasParams()) {
        error = "HudCard.hlsl declares a Params cbuffer; the overlay must read only b0 and b2";
        return false;
    }
    hudReady_ = true;
    Info(MOD, "status card overlay ready");
    return true;
}

// b0 and b2 already hold this frame's values, uploaded by the wallpaper pass that precedes this
// one, so the overlay binds and draws and never touches a constant buffer - which is also why it
// has no Params of its own: paramCb_ is one shared slot resized by byte count, and a second pass
// with a different size would reallocate it every frame of every monitor.
bool D3D11Renderer::DrawHud(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h,
                            std::string& error) {
    if (!hudReady_ || !rtv) { error = "status card overlay not ready"; return false; }
    const float factor[4] = { 0, 0, 0, 0 };
    D3D11_VIEWPORT vp{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetState(raster_.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetDepthStencilState(noDepth_.Get(), 0);
    ctx->OMSetBlendState(over_.Get(), factor, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(hud_.vs(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    ctx->VSSetConstantBuffers(2, 1, statsCb_.GetAddressOf());
    ctx->PSSetShader(hud_.ps(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    ctx->PSSetConstantBuffers(2, 1, statsCb_.GetAddressOf());
    // Slot 0 stays empty so the atlas lands on t1, where Glyphs.hlsl declares it.
    ID3D11ShaderResourceView* srvs[2] = { nullptr, glyphs_.Get() };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[2] = { wrap_.Get(), wrap_.Get() };
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->Draw(3, 0);

    // Unbind, so the next wallpaper's pass cannot read the overlay's atlas through a slot it thinks
    // is still its own - Pass() only ever sets the slots it counts.
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, none);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->VSSetShader(nullptr, nullptr, 0);
    return true;
}

} // namespace sw
