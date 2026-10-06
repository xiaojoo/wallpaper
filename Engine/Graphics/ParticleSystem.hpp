#pragma once
// Engine/Graphics/ParticleSystem.hpp - the GPU-side particle state. The CPU touches this buffer
// once (seed) and never per frame; the compute shader owns all motion.
#include "Engine/Core/Platform.hpp"

namespace sw {

class ParticleSystem {
public:
    static constexpr UINT Stride = 48; // must match struct Particle in Shaders/ParticleCommon.hlsl
    static constexpr UINT Threads = 64; // must match the [numthreads] the contract asks every CSMain for
    static constexpr UINT MaxCount = 250000; // 12 MB at Stride: a wallpaper cannot ask for more

    bool Init(ID3D11Device* dev, UINT count, std::string& error);
    void Seed(ID3D11DeviceContext* ctx, UINT count);

    UINT count() const { return count_; }
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    ID3D11UnorderedAccessView* uav() const { return uav_.Get(); }

    // Proof that the simulation actually runs: hash the positions and count how many left the
    // allowed band. Called from the self test, not from the frame loop.
    bool Sample(ID3D11DeviceContext* ctx, unsigned long long& hash, unsigned& outOfBounds, UINT take);

private:
    Com<ID3D11Buffer> buf_;
    Com<ID3D11ShaderResourceView> srv_;
    Com<ID3D11UnorderedAccessView> uav_;
    Com<ID3D11Buffer> staging_;
    UINT count_ = 0;
};

} // namespace sw
