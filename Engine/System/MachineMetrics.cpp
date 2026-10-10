#include "Engine/System/MachineMetrics.hpp"
#include <cmath>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace sw {
namespace {

double WallSeconds() {
    LARGE_INTEGER f{}, t{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// The path is built in English and added with PdhAddEnglishCounterW, which is what keeps this working
// on a zh-CN install where the same counter is presented to the user under a Chinese name. The numeric
// instance names come from the logical processor count rather than from PdhEnumObjectItemsW, because
// that enumeration takes a *localized* object name and "Processor" is exactly the kind of name that
// differs; "0".."31" do not.
bool AddEnglish(PDH_HQUERY q, const wchar_t* path, PDH_HCOUNTER* out) {
    PDH_HCOUNTER c = nullptr;
    if (FAILED(PdhAddEnglishCounterW(q, path, 0, &c)) || !c) return false;
    *out = c;
    return true;
}

} // namespace

MachineMetrics::~MachineMetrics() {
    if (query_) PdhCloseQuery(query_);
}

bool MachineMetrics::BuildQuery() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    coreCount_ = unsigned(si.dwNumberOfProcessors);
    if (!coreCount_) coreCount_ = 1;
    if (coreCount_ > kMaxCores) coreCount_ = kMaxCores;

    if (FAILED(PdhOpenQueryW(nullptr, 0, &query_))) return false;
    AddEnglish(query_, L"\\Processor(_Total)\\% Processor Time", &cpuTotal_);
    AddEnglish(query_, L"\\System\\Processor Queue Length", &queue_);
    AddEnglish(query_, L"\\System\\Processes", &procs_);
    AddEnglish(query_, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &diskRead_);
    AddEnglish(query_, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &diskWrite_);
    AddEnglish(query_, L"\\PhysicalDisk(_Total)\\% Idle Time", &diskIdle_);
    coreIdle_.assign(coreCount_, nullptr);
    for (unsigned i = 0; i < coreCount_; ++i) {
        const std::wstring path = L"\\Processor(" + std::to_wstring(i) + L")\\% Idle Time";
        AddEnglish(query_, path.c_str(), &coreIdle_[i]);
    }
    // Establishes the base every rate and every % counter needs. Nothing read before this second
    // collect means anything, so the first Sample() after a build reports what it can from the
    // non-PDH sources and leaves the rest at -1.
    PdhCollectQueryData(query_);
    primed_ = false;
    return true;
}

float MachineMetrics::Read(PDH_HCOUNTER c, bool* ok) const {
    if (!c) { if (ok) *ok = false; return -1.f; }
    PDH_FMT_COUNTERVALUE v{};
    if (FAILED(PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v)) || v.CStatus != 0) {
        if (ok) *ok = false;
        return -1.f;
    }
    if (ok) *ok = true;
    return float(v.doubleValue);
}

MachineStats MachineMetrics::Sample(const GpuUse& gpu) {
    MachineStats s;
    s.gpu = gpu;
    if (!query_ && !BuildQuery()) return s;

    const double wall = WallSeconds();
    s.ageS = 0.f;

    // ---- whole-machine CPU, from GetSystemTimes rather than from PDH ------------------------------
    // Two instruments for one quantity on purpose. PDH's \Processor(_Total)\% Processor Time is also
    // read below; if the two ever disagree the panel can say which one it is showing, and a wrong
    // counter path shows up as a mismatch instead of as a plausible number.
    FILETIME fi{}, fk{}, fu{};
    if (GetSystemTimes(&fi, &fk, &fu)) {
        auto bits = [](const FILETIME& f) {
            return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) |
                   static_cast<unsigned long long>(f.dwLowDateTime);
        };
        const unsigned long long i = bits(fi), k = bits(fk), u = bits(fu);
        if (haveTimes_) {
            const double di = double(i - idle_);
            const double dk = double(k - kernel_);
            const double du = double(u - user_);
            const double tot = dk + du;      // kernel time includes idle time on Windows
            if (tot > 0.0) {
                s.cpuPct = float(100.0 * (1.0 - di / tot));
                s.cpuUserPct = float(100.0 * du / tot);
                s.cpuPrivPct = float(100.0 * (dk - di) / tot);
                s.valid |= MachineStats::kCpu;
            }
        }
        idle_ = i; kernel_ = k; user_ = u; haveTimes_ = true;
    }

    // ---- memory, commit, uptime, disk capacity: all one call each --------------------------------
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        s.memTotalMb = float(double(ms.ullTotalPhys) / (1024.0 * 1024.0));
        s.memUsedMb = float(double(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0));
        s.memPct = float(ms.dwMemoryLoad);
        s.commitMb = float(double(ms.ullTotalPageFile - ms.ullAvailPageFile) / (1024.0 * 1024.0));
        s.valid |= MachineStats::kMemory;
    }
    s.uptimeS = float(GetTickCount64()) / 1000.f;
    s.valid |= MachineStats::kSystem;

    wchar_t sysRoot[MAX_PATH]{};
    if (GetSystemWindowsDirectoryW(sysRoot, MAX_PATH) && *sysRoot) {
        sysRoot[3] = L'\\'; sysRoot[4] = 0;         // "C:\Windows" -> "C:\"
        ULARGE_INTEGER free{}, total{};
        if (GetDiskFreeSpaceExW(sysRoot, nullptr, &total, &free)) {
            s.diskTotalGb = float(double(total.QuadPart) / (1024.0 * 1024.0 * 1024.0));
            s.diskFreeGb = float(double(free.QuadPart) / (1024.0 * 1024.0 * 1024.0));
        }
    }

    // ---- network: octet counters over every connected, non-loopback interface ---------------------
    PMIB_IF_TABLE2 tab = nullptr;
    if (GetIfTable2(&tab) == NO_ERROR && tab) {
        unsigned long long in = 0, out = 0;
        for (ULONG i = 0; i < tab->NumEntries; ++i) {
            const MIB_IF_ROW2& r = tab->Table[i];
            // MIB_IF_ROW2 has no MediaState; the pair is MediaConnectState (is a cable/link is up) and
            // OperStatus (is the stack using it). Loopback is excluded because its counters move with
            // every localhost connection this machine makes and would read as network traffic.
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (r.MediaConnectState != MediaConnectStateConnected) continue;
            if (r.OperStatus != NET_IF_OPER_STATUS_UP) continue;
            in += r.InOctets; out += r.OutOctets;
        }
        FreeMibTable(tab);
        if (haveNet_ && wall > lastNetS_) {
            const double dt = wall - lastNetS_;
            s.netRecvBs = float(double(in - netIn_) / dt);
            s.netSendBs = float(double(out - netOut_) / dt);
            s.valid |= MachineStats::kNetwork;
        }
        netIn_ = in; netOut_ = out; haveNet_ = true; lastNetS_ = wall;
    }

    // ---- PDH: needs a window between two collects, so the first pass after a build is skipped -----
    PdhCollectQueryData(query_);
    if (primed_) {
        bool ok = false;
        const float pdhTotal = Read(cpuTotal_, &ok);
        if (ok) {
            // Kept as its own field rather than folded into cpuPct: two instruments reading one
            // quantity are only worth having if a disagreement stays visible. A wallpaper draws
            // cpuPct; the status log and the verification both compare the two.
            s.cpuPctPdh = pdhTotal;
            if (s.cpuPct < 0.f) s.cpuPct = pdhTotal;
        }
        if (float q = Read(queue_, &ok); ok) s.cpuQueue = q;
        if (float p = Read(procs_, &ok); ok) s.processCount = p;
        if (float r = Read(diskRead_, &ok); ok) {
            s.diskReadBs = r;
            bool w2 = false;
            if (float w = Read(diskWrite_, &w2); w2) s.diskWriteBs = w;
            bool i2 = false;
            if (float idlePct = Read(diskIdle_, &i2); i2) s.diskActivePct = 100.f - idlePct;
            s.valid |= MachineStats::kDisk;
        }
        unsigned got = 0;
        for (unsigned i = 0; i < coreCount_; ++i) {
            bool cok = false;
            if (float idlePct = Read(coreIdle_[i], &cok); cok) { s.core[i] = 100.f - idlePct; ++got; }
            else s.core[i] = -1.f;
        }
        if (got) { s.coreCount = got; s.valid |= MachineStats::kCores; }
    } else {
        for (unsigned i = 0; i < kMaxCores; ++i) s.core[i] = -1.f;
        primed_ = true;
    }
    if (s.gpu.device >= 0.0) s.valid |= MachineStats::kGpu;
    return s;
}

void FillStatsCB(const MachineStats& x, StatsCB& out) {
    out = StatsCB();               // start from all -1; only answered fields are written over it
    out.cpu[0] = x.cpuPct; out.cpu[1] = x.cpuPctPdh;
    out.cpu[2] = x.cpuUserPct; out.cpu[3] = x.cpuPrivPct;
    out.memory[0] = x.memPct; out.memory[1] = x.memUsedMb;
    out.memory[2] = x.memTotalMb; out.memory[3] = x.commitMb;
    out.gpu[0] = x.gpu.device; out.gpu[1] = x.gpu.memUtilPct;
    out.gpu[2] = x.gpu.memUsedMb; out.gpu[3] = x.gpu.memTotalMb;
    out.gpu2[0] = x.gpu.tempC; out.gpu2[1] = x.gpu.powerW; out.gpu2[2] = x.gpu.clockMHz;
    out.disk[0] = x.diskReadBs; out.disk[1] = x.diskWriteBs;
    out.disk[2] = x.diskActivePct; out.disk[3] = x.cpuQueue;
    out.disk2[0] = x.diskFreeGb; out.disk2[1] = x.diskTotalGb;
    out.disk2[2] = x.processCount; out.disk2[3] = x.uptimeS;
    out.net[0] = x.netRecvBs; out.net[1] = x.netSendBs;
    out.meta[0] = float(x.valid); out.meta[1] = x.ageS; out.meta[2] = float(x.coreCount);
    for (int i = 0; i < kMaxCores; ++i) out.core[i] = x.core[i];
}

} // namespace sw
