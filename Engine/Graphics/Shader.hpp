#pragma once
// Engine/Graphics/Shader.hpp - runtime HLSL compile + wallpaper parameter reflection.
#include "Engine/Core/Platform.hpp"
#include <map>

namespace sw {

// b1 cbuffer "Params" is filled from wallpaper.json "params" by reflected name.
// Scalars, vectors, bools, ints and 1-component arrays are supported.
class Shader {
public:
    struct Field {
        std::string name;
        UINT offset = 0;
        UINT components = 1; // floats written per element
        UINT elements = 1;   // array length
        UINT stride = 4;     // bytes between array elements
        bool isInt = false;
    };

    // A particle wallpaper uses all five: the compute shader integrates the state, `vs` + `bg`
    // draw the background as one fullscreen triangle, `pvs` + `ps` draw the particles over it.
    struct Entries {
        std::string vs = "VSMain", ps = "PSMain";
        std::string cs;    // compute: particle simulation
        std::string bg;    // pixel: background pass, uses the fullscreen vs
        std::string pvs;   // vertex: expands each particle into triangles
    };

    static bool Available(std::string& why);

    bool Compile(const std::string& source, const std::wstring& fileNameForErrors,
                 const std::vector<std::wstring>& includeDirs, const Entries& entries, std::string& error);
    bool CreatePipeline(ID3D11Device* dev, std::string& error);
    bool ReflectParams(std::string& error);

    const std::vector<Field>& fields() const { return fields_; }
    UINT paramBytes() const { return paramBytes_; }
    bool hasParams() const { return !fields_.empty(); }

    // Writes every reflected field from the JSON object. Keys that matched no field land in `unusedOut`.
    bool FillParams(const Json& params, void* dst, size_t dstBytes, std::vector<std::string>& unusedOut) const;

    ID3D11VertexShader* vs() const { return vs_.Get(); }
    ID3D11PixelShader* ps() const { return ps_.Get(); }
    ID3D11ComputeShader* cs() const { return cs_.Get(); }
    ID3D11PixelShader* backgroundPs() const { return bg_.Get(); }
    ID3D11VertexShader* particleVs() const { return pvs_.Get(); }

private:
    Com<ID3DBlob> vsBlob_, psBlob_, csBlob_, bgBlob_, pvsBlob_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11ComputeShader> cs_;
    Com<ID3D11PixelShader> bg_;
    Com<ID3D11VertexShader> pvs_;
    std::vector<Field> fields_;
    UINT paramBytes_ = 0;
};

} // namespace sw
