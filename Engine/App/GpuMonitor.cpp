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
    if (!getRates_) { device_ = nullptr; return; }

    // Each of these stays optional on purpose. A driver that will not answer for power or for the
    // temperature of a fanless card is normal, and Sample() reports those as -1; dropping device_ over
    // one missing field would take the utilisation number down with it, which is the one we already
    // show in the status line.
    getMem_ = reinterpret_cast<int (*)(void*, Memory*)>(GetProcAddress(lib_, "nvmlDeviceGetMemoryInfo"));
    getMem2_ = reinterpret_cast<int (*)(void*, Memory2*)>(GetProcAddress(lib_, "nvmlDeviceGetMemoryInfo_v2"));
    getTemp_ = reinterpret_cast<int (*)(void*, unsigned, unsigned*)>(
        GetProcAddress(lib_, "nvmlDeviceGetTemperature"));
    getPower_ = reinterpret_cast<int (*)(void*, unsigned*)>(GetProcAddress(lib_, "nvmlDeviceGetPowerUsage"));
    getClock_ = reinterpret_cast<int (*)(void*, unsigned, unsigned*)>(
        GetProcAddress(lib_, "nvmlDeviceGetClockInfo"));
    getName_ = reinterpret_cast<int (*)(void*, char*, unsigned)>(GetProcAddress(lib_, "nvmlDeviceGetName"));
    if (getName_) {
        char buf[128]{};
        if (getName_(device_, buf, sizeof(buf)) == 0) name_ = buf;
    }
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
    if (device_ && getRates_ && getRates_(device_, &v) == 0) {
        u.device = double(v.gpu);
        u.memUtilPct = double(v.memory);
    }
    Memory2 m2{};
    m2.version = 2;
    bool gotV2 = false;
    // v2 first, and only its `used` - that is the number nvidia-smi and Task Manager print. The
    // version field is an input as well as an output: leaving it unset makes the call answer
    // NVML_ERROR_ARGUMENT_VERSION_MISMATCH and fall through to v1, which is the wrong number.
    if (device_ && getMem2_ && getMem2_(device_, &m2) == 0 && m2.total) {
        u.memUsedMb = double(m2.used) / (1024.0 * 1024.0);
        u.memTotalMb = double(m2.total) / (1024.0 * 1024.0);
        u.memReservedMb = double(m2.reserved) / (1024.0 * 1024.0);
        gotV2 = true;
    }
    if (!gotV2) {
        Memory mem{};
        if (device_ && getMem_ && getMem_(device_, &mem) == 0 && mem.total) {
            u.memUsedMb = double(mem.used) / (1024.0 * 1024.0);
            u.memTotalMb = double(mem.total) / (1024.0 * 1024.0);
        }
    }
    unsigned n = 0;
    // NVML_TEMPERATURE_GPU is 0, and NVML_CLOCK_SM is 1 - spelled as numbers here because nothing is
    // built against nvml.h, same reason the entry points above are declared by hand.
    if (device_ && getTemp_ && getTemp_(device_, 0, &n) == 0) u.tempC = double(n);
    // Milliwatts, not microwatts: dividing by 1e6 here made a card drawing 29.4 W report 0.0, which is
    // indistinguishable from an unmeasured field and from an idle card. Found by reading the same
    // quantity from nvidia-smi at the same moment.
    if (device_ && getPower_ && getPower_(device_, &n) == 0) u.powerW = double(n) / 1000.0;
    if (device_ && getClock_ && getClock_(device_, 1, &n) == 0) u.clockMHz = double(n);
    u.name = name_;

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
        // Only the per-engine self numbers are repeated; everything the card says about itself is
        // already fresh in u, and repeating it too would age the temperature by a second for no reason.
        r.device = u.device; r.memUtilPct = u.memUtilPct;
        r.memUsedMb = u.memUsedMb; r.memTotalMb = u.memTotalMb;
        r.tempC = u.tempC; r.powerW = u.powerW; r.clockMHz = u.clockMHz;
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
        // last_ holds only what has to be repeated: the self numbers. Every card-side field is cleared
        // so a repeat cannot serve a temperature that is two seconds old while claiming it is live.
        last_.device = last_.memUtilPct = last_.memUsedMb = last_.memTotalMb = -1.0;
        last_.tempC = last_.powerW = last_.clockMHz = -1.0;
    }
    return u;
}

} // namespace sw
