#include "Engine/Graphics/Shader.hpp"
#include "Engine/Core/Log.hpp"

#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <cmath>

namespace sw {
static constexpr const char* MOD = "shader";

namespace {

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, CONST D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3DReflect)(LPCVOID, SIZE_T, REFIID, void**);

struct CompilerApi {
    HMODULE mod = nullptr;
    PFN_D3DCompile compile = nullptr;
    PFN_D3DReflect reflect = nullptr;
};

// Loaded dynamically: d3dcompiler_47.dll ships in System32, so no redistributable is needed.
CompilerApi LoadCompiler() {
    static CompilerApi api = [] {
        CompilerApi a;
        a.mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (a.mod) {
            a.compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(a.mod, "D3DCompile"));
            a.reflect = reinterpret_cast<PFN_D3DReflect>(GetProcAddress(a.mod, "D3DReflect"));
        }
        return a;
    }();
    return api;
}

// Resolves #include "..." against the wallpaper folder and the shared Shaders folder.
class IncludeResolver : public ID3DInclude {
public:
    std::vector<std::wstring> dirs;

    STDMETHOD(Open)(THIS_ D3D_INCLUDE_TYPE type, LPCSTR fileName, LPCVOID parent, LPCVOID* data, UINT* bytes) override {
        (void)type;
        (void)parent;
        *data = nullptr;
        *bytes = 0;
        if (!fileName) return E_INVALIDARG;
        // The compiler hands us bytes; wallpaper manifests use UTF-8 paths, so decode as UTF-8.
        std::wstring name = ToWide(fileName);
        for (auto& d : dirs) {
            std::wstring p = d + L"\\" + name;
            HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz{};
            if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1LL << 22)) {
                CloseHandle(h);
                continue;
            }
            std::string buf((size_t)sz.QuadPart, '\0');
            DWORD got = 0;
            BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
            CloseHandle(h);
            if (!ok || got != buf.size()) continue;
            char* heap = new char[buf.size()]; // the compiler frees it through Close()
            memcpy(heap, buf.data(), buf.size());
            *data = heap;
            *bytes = (UINT)buf.size();
            return S_OK;
        }
        Warn(MOD, "hlsl include not found: {}", ToUtf8(name));
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    STDMETHOD(Close)(THIS_ LPCVOID data) override {
        delete[] static_cast<const char*>(data);
        return S_OK;
    }
};

std::string BlobError(ID3DBlob* err) {
    if (!err) return "(no detail)";
    return std::string(static_cast<char*>(err->GetBufferPointer()), err->GetBufferSize());
}

} // namespace

bool Shader::Available(std::string& why) {
    auto api = LoadCompiler();
    if (!api.compile || !api.reflect) {
        why = "d3dcompiler_47.dll (D3DCompile/D3DReflect) could not be loaded";
        return false;
    }
    return true;
}

bool Shader::Compile(const std::string& source, const std::wstring& fileNameForErrors,
                     const std::vector<std::wstring>& includeDirs, const Entries& e, std::string& error) {
    const std::string vsEntry = e.vs, psEntry = e.ps;
    auto api = LoadCompiler();
    if (!api.compile) {
        error = "d3dcompiler_47.dll missing";
        return false;
    }
    IncludeResolver inc;
    inc.dirs = includeDirs;
    std::string name = ToUtf8(fileNameForErrors);
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    Com<ID3DBlob> err;
    HRESULT hr = api.compile(source.data(), source.size(), name.c_str(), nullptr, &inc, vsEntry.c_str(), "vs_5_0",
                             flags, 0, &vsBlob_, &err);
    if (FAILED(hr)) {
        error = "vertex shader (" + vsEntry + "):\n" + BlobError(err.Get());
        return false;
    }
    hr = api.compile(source.data(), source.size(), name.c_str(), nullptr, &inc, psEntry.c_str(), "ps_5_0", flags, 0,
                    &psBlob_, &err);
    if (FAILED(hr)) {
        error = "pixel shader (" + psEntry + "):\n" + BlobError(err.Get());
        return false;
    }
    if (!e.cs.empty()) {
        // fxc only declares RWStructuredBuffer for cs_5_0, so the shared particle header needs to
        // know which stage it is being read for.
        static const D3D_SHADER_MACRO csDefines[] = { {"SW_COMPUTE", "1"}, {nullptr, nullptr} };
        hr = api.compile(source.data(), source.size(), name.c_str(), csDefines, &inc, e.cs.c_str(), "cs_5_0", flags,
                         0, &csBlob_, &err);
        if (FAILED(hr)) {
            error = "compute shader (" + e.cs + "):\n" + BlobError(err.Get());
            return false;
        }
    }
    if (!e.pvs.empty()) {
        hr = api.compile(source.data(), source.size(), name.c_str(), nullptr, &inc, e.pvs.c_str(), "vs_5_0", flags, 0,
                         &pvsBlob_, &err);
        if (FAILED(hr)) {
            error = "particle vertex shader (" + e.pvs + "):\n" + BlobError(err.Get());
            return false;
        }
    }
    if (!e.bg.empty()) {
        hr = api.compile(source.data(), source.size(), name.c_str(), nullptr, &inc, e.bg.c_str(), "ps_5_0", flags, 0,
                         &bgBlob_, &err);
        if (FAILED(hr)) {
            error = "background pixel shader (" + e.bg + "):\n" + BlobError(err.Get());
            return false;
        }
    }
    Info(MOD, "compiled {}: vs {}B, ps {}B{}{}{}", ToUtf8(fileNameForErrors), (unsigned)vsBlob_->GetBufferSize(),
         (unsigned)psBlob_->GetBufferSize(), csBlob_ ? ", cs " : "", csBlob_ ? std::to_string((unsigned)csBlob_->GetBufferSize()).c_str() : "",
         pvsBlob_ ? ", pvs+bg" : "");
    return true;
}

bool Shader::CreatePipeline(ID3D11Device* dev, std::string& error) {
    if (!vsBlob_ || !psBlob_) {
        error = "shader not compiled";
        return false;
    }
    HRESULT hr = dev->CreateVertexShader(vsBlob_->GetBufferPointer(), vsBlob_->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) {
        error = "CreateVertexShader: " + HResultToString(hr);
        return false;
    }
    hr = dev->CreatePixelShader(psBlob_->GetBufferPointer(), psBlob_->GetBufferSize(), nullptr, &ps_);
    if (FAILED(hr)) {
        error = "CreatePixelShader: " + HResultToString(hr);
        return false;
    }
    if (csBlob_) {
        hr = dev->CreateComputeShader(csBlob_->GetBufferPointer(), csBlob_->GetBufferSize(), nullptr, &cs_);
        if (FAILED(hr)) {
            error = "CreateComputeShader: " + HResultToString(hr);
            return false;
        }
    }
    if (pvsBlob_) {
        hr = dev->CreateVertexShader(pvsBlob_->GetBufferPointer(), pvsBlob_->GetBufferSize(), nullptr, &pvs_);
        if (FAILED(hr)) {
            error = "CreateVertexShader(pvs): " + HResultToString(hr);
            return false;
        }
    }
    if (bgBlob_) {
        hr = dev->CreatePixelShader(bgBlob_->GetBufferPointer(), bgBlob_->GetBufferSize(), nullptr, &bg_);
        if (FAILED(hr)) {
            error = "CreatePixelShader(bg): " + HResultToString(hr);
            return false;
        }
    }
    return true;
}

bool Shader::ReflectParams(std::string& error) {
    fields_.clear();
    paramBytes_ = 0;
    auto api = LoadCompiler();
    if (!api.reflect) {
        error = "D3DReflect missing";
        return false;
    }
    Com<ID3D11ShaderReflection> refl;
    HRESULT hr = api.reflect(psBlob_->GetBufferPointer(), psBlob_->GetBufferSize(), IID_PPV_ARGS(&refl));
    if (FAILED(hr)) {
        error = "D3DReflect: " + HResultToString(hr);
        return false;
    }
    D3D11_SHADER_DESC desc{};
    refl->GetDesc(&desc);
    int paramsBuffer = -1;
    for (UINT i = 0; i < desc.ConstantBuffers; ++i) {
        auto* cb = refl->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cbd{};
        cb->GetDesc(&cbd);
        std::string nm = cbd.Name ? cbd.Name : "";
        if (nm == "Params") paramsBuffer = (int)i;
        else Info(MOD, "cbuffer {} ({} B, {} var) is engine-owned", nm, cbd.Size, cbd.Variables);
    }
    if (paramsBuffer < 0) return true;

    auto* cb = refl->GetConstantBufferByIndex((UINT)paramsBuffer);
    D3D11_SHADER_BUFFER_DESC cbd{};
    cb->GetDesc(&cbd);
    paramBytes_ = cbd.Size;
    for (UINT v = 0; v < cbd.Variables; ++v) {
        auto* var = cb->GetVariableByIndex(v);
        D3D11_SHADER_VARIABLE_DESC vd{};
        var->GetDesc(&vd);
        D3D11_SHADER_TYPE_DESC td{};
        var->GetType()->GetDesc(&td);
        std::string nm = vd.Name ? vd.Name : "?";

        Field f;
        f.name = nm;
        f.offset = vd.StartOffset;
        f.elements = td.Elements ? td.Elements : 1;
        if (td.Rows > 1) {
            Warn(MOD, "Params.{} is a matrix - not bindable from JSON, skipped", nm);
            continue;
        }
        switch (td.Type) {
            case D3D_SVT_FLOAT: f.isInt = false; break;
            case D3D_SVT_INT:
            case D3D_SVT_UINT:
            case D3D_SVT_BOOL: f.isInt = true; break;
            default:
                Warn(MOD, "Params.{} has unsupported HLSL type {} - skipped", nm, (int)td.Type);
                continue;
        }
        f.components = td.Columns ? td.Columns : 1;
        f.stride = f.elements > 1 && vd.Size >= f.elements ? (UINT)(vd.Size / f.elements) : 4;
        if (f.offset % 4 || f.stride % 4) {
            Warn(MOD, "Params.{} offset/stride not 4-byte aligned - skipped", nm);
            continue;
        }
        fields_.push_back(std::move(f));
    }
    Info(MOD, "Params cbuffer: {} B, {} bindable field(s)", paramBytes_, (unsigned)fields_.size());
    for (auto& f : fields_)
        Info(MOD, "  {} = {} x float{} (offset {}, stride {})", f.name, f.elements, f.components, f.offset, f.stride);
    return true;
}

bool Shader::FillParams(const Json& params, void* dst, size_t dstBytes, std::vector<std::string>& unusedOut) const {
    memset(dst, 0, dstBytes);
    float* base = static_cast<float*>(dst);
    const size_t floats = dstBytes / 4;
    bool any = false;

    for (auto& f : fields_) {
        const Json* value = nullptr;
        for (auto& kv : params.members()) {
            if (kv.first == f.name || kv.first == "u" + f.name ||
                (!f.name.empty() && f.name[0] == 'u' && kv.first == f.name.substr(1))) {
                value = &kv.second;
                break;
            }
        }
        if (!value) continue;
        any = true;
        if (value->isString()) {
            Warn(MOD, "Params.{} is a string in JSON - only numbers/bools/arrays bind", f.name);
            continue;
        }

        const size_t maxFloatsPerElem = f.components;
        auto writeElem = [&](UINT elem, const Json& cell) {
            size_t byteOff = f.offset + (size_t)elem * f.stride;
            if (byteOff + maxFloatsPerElem * 4 > dstBytes) return;
            float* slot = base + byteOff / 4;
            for (UINT c = 0; c < f.components; ++c) {
                const Json* cell2 = &cell;
                if (cell.isArray()) {
                    if (c >= cell.items().size()) break;
                    cell2 = &cell.items()[c];
                }
                if (f.isInt) {
                    *(int*)slot = (int)std::llround(cell2->asNumber(cell2->asBool() ? 1.0 : 0.0));
                } else {
                    *slot = (float)cell2->asNumber(cell2->asBool() ? 1.0 : 0.0);
                }
                ++slot;
            }
        };

        if (f.elements > 1 && value->isArray()) {
            for (size_t e = 0; e < f.elements && e < value->items().size(); ++e) writeElem((UINT)e, value->items()[e]);
        } else if (f.elements > 1) {
            for (UINT e = 0; e < f.elements; ++e) writeElem(e, *value); // one JSON number fills the whole array
        } else {
            writeElem(0, *value);
        }
        (void)floats;
    }

    for (auto& kv : params.members()) {
        bool bound = false;
        for (auto& f : fields_)
            if (kv.first == f.name || kv.first == "u" + f.name ||
                (!f.name.empty() && f.name[0] == 'u' && kv.first == f.name.substr(1))) bound = true;
        if (!bound) unusedOut.push_back(kv.first);
    }
    return any;
}

} // namespace sw
