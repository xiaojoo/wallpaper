#pragma once
// Engine/Graphics/D3D11Renderer.hpp - DirectComposition surface per monitor + the fullscreen draw call.
#include "Engine/Core/Platform.hpp"
#include "Engine/Graphics/D3D11Device.hpp" // FrameCB
#include "Engine/Graphics/Shader.hpp"      // the overlay's compiled pipeline
#include "Engine/Graphics/Texture.hpp"     // its glyph atlas

namespace sw {

// One composition visual tree attached to one desktop window.
// Surfaces (not swap chains) are required here: a flip-model swap chain cannot be created
// for the child window we host under WorkerW.
class RenderSurface {
public:
    bool Init(ID3D11Device* dev, IDCompositionDevice* comp, HWND hwnd, UINT w, UINT h, std::string& error);
    bool Resize(UINT w, UINT h, std::string& error);
    void Release();

    // Locks the surface, binds it as the render target, and hands back what the draw needs.
    bool Begin(std::string& error);
    void End(ID3D11DeviceContext* ctx);

    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    // Copies one corner of the last drawn surface into CPU memory and returns a checksum.
    // This is the in-process proof that a wallpaper is actually changing pixels. `corner` is
    // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right - the caller picks one that the
    // engine's own overlay is not sitting on.
    bool SampleCorner(ID3D11DeviceContext* ctx, unsigned long long& checksum, UINT side = 16,
                      UINT corner = 0);
    UINT width() const { return w_; }
    UINT height() const { return h_; }
    bool valid() const { return surface_ != nullptr; }
    void setLabel(std::wstring label) { label_ = std::move(label); }
    const std::wstring& label() const { return label_; }

private:
    Com<IDCompositionDevice> comp_;
    Com<IDCompositionTarget> target_;
    Com<IDCompositionVisual> visual_;
    Com<IDCompositionSurface> surface_;
    Com<ID3D11Texture2D> drawn_;
    Com<ID3D11Texture2D> readback_;
    Com<ID3D11RenderTargetView> rtv_;
    ID3D11Device* dev_ = nullptr;
    UINT w_ = 0, h_ = 0;
    std::wstring label_;
};

struct DrawArgs {
    FrameCB frame{};
    // What the machine is doing, for a package that reads uStats. Default-constructed to -1 rather
    // than 0, so a path that never fills it (a thumbnail, for one) shows a wallpaper an unmeasured
    // machine rather than an idle one.
    StatsCB stats;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* srvs[8] = {};
    UINT srvCount = 0;
    const void* params = nullptr;
    UINT paramBytes = 0;
    // How this pass composites onto what is already in the render target.
    //   Opaque   - the wallpaper's own pass; it covers the surface.
    //   Additive - ONE/ONE, for glowing flakes over a background pass. It can only ever lighten.
    //   Over     - premultiplied SRC_ALPHA, for an opaque panel with a soft edge: the engine's own
    //              status card. Additive cannot do that job, because the card's dark fill drawn
    //              with ONE/ONE leaves the wallpaper showing straight through it.
    enum class Blend : unsigned char { Opaque, Additive, Over };
    Blend blend = Blend::Opaque;

    // Particle wallpapers run three passes instead of one. `cs` integrates the state into `uav`,
    // `vs` + `backgroundPs` draw the sky, and `particleVs` + `ps` draw the particles over it.
    // The particle buffer takes t0 in that last pass, so the package's own textures shift to t1..
    // and the vertex count comes from the buffer, never from the manifest.
    ID3D11ComputeShader* cs = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11ShaderResourceView* particles = nullptr;
    ID3D11VertexShader* particleVs = nullptr;
    ID3D11PixelShader* backgroundPs = nullptr;
    UINT particleCount = 0;
    UINT verticesPerParticle = 6;   // six, because D3D11 points are fixed at 1x1 pixel
    UINT threadsPerGroup = 64;      // the [numthreads] the engine's particle contract asks for
};

// Owns the state objects that every monitor shares: constant buffers, blend/raster states, samplers.
class D3D11Renderer {
public:
    bool Init(ID3D11Device* dev, std::string& error);

    // One wallpaper frame. A package with particles dispatches the compute shader and draws two
    // passes; everything else is a single fullscreen triangle. This is what the frame loop calls.
    bool Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h, const DrawArgs& args,
                std::string& error);

    // The engine's own overlay: compiles Shaders/HudCard.hlsl and loads Shaders/glyphs.png once,
    // for the life of the process rather than per slot, because the card is not a wallpaper and
    // must survive every apply, reload and monitor change. Safe to leave un-called: DrawHud then
    // reports not-ready and the desktop simply draws the wallpaper alone.
    bool InitHud(ID3D11Device* dev, IWICImagingFactory* wic, const std::wstring& root, std::string& error);

    // Draws the card over whatever is already in `rtv`. Assumes b0 and b2 already hold this
    // frame's values - the wallpaper pass that precedes it uploaded them - so it binds and draws
    // and touches no constant buffer. Call it after Render(), inside the same BeginDraw/EndDraw.
    bool DrawHud(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h, std::string& error);

    bool hudReady() const { return hudReady_; }

private:
    // The fullscreen triangle the wallpaper's PS runs over.
    bool Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h, const DrawArgs& args,
              std::string& error);
    // Six vertices per particle, expanded in the vertex shader from the structured buffer.
    bool DrawParticles(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h, const DrawArgs& args,
                       std::string& error);
    // Integrates the particle state. The UAV is unbound again before returning: leaving the buffer
    // bound for writing while the vertex shader reads it is undefined behaviour.
    bool Dispatch(ID3D11DeviceContext* ctx, const DrawArgs& args, std::string& error);

    // The shared binding code. `srvs`/`srvCount` differ per pass because the particle buffer takes t0.
    bool Pass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h, const DrawArgs& args,
              ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srvs, UINT srvCount,
              UINT vertexCount, std::string& error);
    bool UploadConstants(ID3D11DeviceContext* ctx, const DrawArgs& args, ID3D11Buffer** paramView,
                         std::string& error);

    Com<ID3D11Buffer> frameCb_;
    Com<ID3D11Buffer> paramCb_;
    UINT paramCbBytes_ = 0;
    Com<ID3D11Buffer> statsCb_;   // b2, fixed size, so unlike paramCb_ it is made once in Init
    Com<ID3D11RasterizerState> raster_;
    Com<ID3D11BlendState> opaque_, additive_, over_;
    Com<ID3D11DepthStencilState> noDepth_;
    Com<ID3D11SamplerState> wrap_, clamp_;
    Shader hud_;
    Com<ID3D11ShaderResourceView> glyphs_;
    bool hudReady_ = false;
};

} // namespace sw
