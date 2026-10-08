#include "Engine/App/GpuMonitor.hpp"
#include <pdh.h>
#include <cwchar>

#pragma comment(lib, "pdh.lib")

namespace sw {
namespace {

// The instance names of the "GPU Engine" counter set, filtered to the ones belonging to one pid.
// PdhGetFormattedCounterArray is not used anywhere in this file: its "ask for the size first" step
// leaves pdwItemCount undefined, and sizing the buffer from what it reported let it write past the end
// (measured: items=58379 against bytes=664, then an access violation). Enumerating the names and adding
// one counter per name with the scalar read avoids that whole path.
std::vector<std::wstring> EngineInstances(unsigned pid) {
    const std::wstring needle = L"pid_" + std::to_wstring(pid) + L"_";
    constexpr DWORD kDetail = 1;   // PDH_DETAIL_ROOT_AND_ITEM_BASENAMES; the SDK spells the enum
                                   // PDH_DETAIL_LEVEL but does not define that name as a constant.
    DWORD cc = 0, ci = 0;
    PdhEnumObjectItemsW(nullptr, nullptr, L"GPU Engine", nullptr, &cc, nullptr, &ci, kDetail, 0);
    if (!ci) return {};
    std::vector<wchar_t> counters(cc ? cc : 1), inst(ci);
    DWORD got = ci;
    if (FAILED(PdhEnumObjectItemsW(nullptr, nullptr, L"GPU Engine", counters.data(), &cc,
                                   inst.data(), &got, kDetail, 0)))
        return {};
    std::vector<std::wstring> out;
    for (size_t i = 0; i + 1 < inst.size() && inst[i]; ) {
        std::wstring name(inst.data() + i);
        if (name.find(needle) != std::wstring::npos) out.push_back(name);
        i += name.size() + 1;
    }
    return out;
}

bool Same(const std::vector<std::wstring>& a, const std::vector<GpuMonitor::Engine>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i].name) return false;
    }
    return true;
}

} // namespace

GpuMonitor::GpuMonitor() {
    // System32 only, so a stray nvml.dll next to the exe cannot be what gets loaded.
    lib_ = LoadLibraryExA("nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) return;
    auto init = reinterpret_cast<int (*)()>(GetProcAddress(lib_, "nvmlInit_v2"));
    if (!init) init = reinterpret_cast<int (*)()>(GetProcAddress(lib_, "nvmlInit"));
    if (!init || init() != 0) return;
    auto byIndex = reinterpret_cast<int (*)(unsigned, void**)>(
        GetProcAddress(lib_, "nvmlDeviceGetHandleByIndex_v2"));
    if (!byIndex) byIndex = reinterpret_cast<int (*)(unsigned, void**)>(
        GetProcAddress(lib_, "nvmlDeviceGetHandleByIndex"));
    if (!byIndex || byIndex(0, &device_) != 0) return;
    getRates_ = reinterpret_cast<int (*)(void*, Util*)>(
        GetProcAddress(lib_, "nvmlDeviceGetUtilizationRates"));
    if (!getRates_) device_ = nullptr;
}

GpuMonitor::~GpuMonitor() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
    if (lib_) {
        if (auto shutdown = reinterpret_cast<int (*)()>(GetProcAddress(lib_, "nvmlShutdown"))) shutdown();
        FreeLibrary(lib_);
    }
}

void GpuMonitor::RebuildEngines() {
    if (query_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
        query_ = nullptr;
    }
    engines_.clear();
    PDH_HQUERY q = nullptr;
    if (FAILED(PdhOpenQueryW(nullptr, 0, &q))) return;
    query_ = q;
    for (auto& name : EngineInstances(GetCurrentProcessId())) {
        const std::wstring path = L"\\GPU Engine(" + name + L")\\Utilization Percentage";
        PDH_HCOUNTER c = nullptr;
        if (FAILED(PdhAddEnglishCounterW(q, path.c_str(), 0, &c)) || !c) continue;
        engines_.push_back({ name, c });
    }
    // The first collect of a fresh query only establishes the base for a rate counter.
    PdhCollectQueryData(q);
}

GpuUse GpuMonitor::Sample() {
    GpuUse u;
    Util v{};
    if (device_ && getRates_ && getRates_(device_, &v) == 0) u.device = double(v.gpu);

    // Our engine list is only rebuilt when the set actually changed, because a rebuild costs one
    // sample of nothing: the counter needs a base before it reports a window.
    auto names = EngineInstances(GetCurrentProcessId());
    if (names.empty()) {
        if (!engines_.empty()) RebuildEngines();
        return u;                       // nothing of ours on any engine right now
    }
    if (!Same(names, engines_)) {
        RebuildEngines();
        stale_ = true;
    }
    if (engines_.empty() || FAILED(PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)))) return u;
    // The first read out of a rebuilt query has no window behind it and answers 0.0 for every engine,
    // which would look like "this program is not using the GPU" for a second. Repeat the previous
    // reading instead - it is one second old at worst, and on the very first pass there is nothing to
    // repeat, so the segment stays out of the line until a real window exists.
    if (stale_) {
        stale_ = false;
        GpuUse r = last_;
        r.device = u.device;            // the card's own number is always fresh; only self is repeated
        return r;
    }

    double sum = 0, d3 = 0, dec = 0;
    bool any = false;
    for (auto& e : engines_) {
        PDH_FMT_COUNTERVALUE val{};
        if (FAILED(PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(e.counter), PDH_FMT_DOUBLE,
                                               nullptr, &val)) || val.CStatus != 0) continue;
        any = true;
        sum += val.doubleValue;
        if (e.name.find(L"engtype_3D") != std::wstring::npos) d3 += val.doubleValue;
        if (e.name.find(L"engtype_VideoDecode") != std::wstring::npos) dec += val.doubleValue;
    }
    if (any) {
        u.self = sum;
        u.self3d = d3;
        u.selfDecode = dec;
        last_ = u;
        last_.device = -1.0;            // only the self readings are repeated; the device is re-read
    }
    return u;
}

} // namespace sw
