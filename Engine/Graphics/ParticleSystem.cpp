#include "Engine/Graphics/ParticleSystem.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "part";

bool ParticleSystem::Init(ID3D11Device* dev, UINT count, std::string& error) {
    count_ = std::min(count ? count : 1u, MaxCount);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = count_ * Stride;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.CPUAccessFlags = 0;
    bd.StructureByteStride = Stride;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &buf_);
    if (FAILED(hr)) {
        error = "particle buffer " + std::to_string(count_) + ": " + HResultToString(hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.NumElements = count_;
    hr = dev->CreateShaderResourceView(buf_.Get(), &sv, &srv_);
    if (FAILED(hr)) { error = "particle srv: " + HResultToString(hr); return false; }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_UNKNOWN;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = count_;
    hr = dev->CreateUnorderedAccessView(buf_.Get(), &uv, &uav_);
    if (FAILED(hr)) { error = "particle uav: " + HResultToString(hr); return false; }

    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = bd.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.StructureByteStride = Stride;
    sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    hr = dev->CreateBuffer(&sd, nullptr, &staging_);
    if (FAILED(hr)) error = "particle staging buffer: " + HResultToString(hr);
    Info(MOD, "particle system ready: {} particles, {} B ({} KB)", count_, count_ * Stride,
         (count_ * Stride) >> 10);
    return SUCCEEDED(hr);
}

// One-time upload so the first frames are not all starting from the top edge. Per-frame motion
// stays on the GPU; this function is never called again.
void ParticleSystem::Seed(ID3D11DeviceContext* ctx, UINT count) {
    std::vector<float> data((size_t)count * 12);
    unsigned x = 0x9E3779B9u;
    auto rnd = [&x]() {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return (double)(x & 0xFFFFFF) / (double)0xFFFFFF;
    };
    for (UINT i = 0; i < count; ++i) {
        float* p = data.data() + (size_t)i * 12;
        const double depth = rnd();
        p[0] = (float)rnd();                       // pos.x
        p[1] = (float)(rnd() * 1.2 - 0.1);         // pos.y
        p[2] = 0.f; p[3] = 0.f;                    // vel
        p[4] = 1.f; p[5] = 1.f; p[6] = 1.f;        // color rgb
        p[7] = (float)(0.35 + depth * 0.65);       // alpha weight
        p[8] = (float)(rnd() * 12.0);              // life
        p[9] = 12.f;                               // maxLife
        p[10] = (float)(1.5 + depth * 7.0);        // size
        p[11] = (float)rnd();                      // seed
    }
    ctx->UpdateSubresource(buf_.Get(), 0, nullptr, data.data(), 0, 0);
    ctx->Flush();
}

bool ParticleSystem::Sample(ID3D11DeviceContext* ctx, unsigned long long& hash, unsigned& outOfBounds, UINT take) {
    hash = 0;
    outOfBounds = 0;
    if (!staging_) return false;
    take = std::min(take, count_);
    ctx->CopyResource(staging_.Get(), buf_.Get());
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mp))) return false;
    const float* p = static_cast<const float*>(mp.pData);
    unsigned long long h = 1469598103934665603ULL;
    const BYTE* bytes = reinterpret_cast<const BYTE*>(p);
    for (UINT i = 0; i < take; ++i) {
        const float* e = p + (size_t)i * 12;
        if (e[0] < -0.25f || e[0] > 1.25f || e[1] > 1.30f) ++outOfBounds;
        for (int b = 0; b < 8; ++b) { h ^= bytes[i * Stride + b]; h *= 1099511628211ULL; }
    }
    ctx->Unmap(staging_.Get(), 0);
    hash = h;
    return true;
}

} // namespace sw
