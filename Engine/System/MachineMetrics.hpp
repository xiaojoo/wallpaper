#pragma once
// Engine/System/MachineMetrics.hpp - what this computer is doing right now, as numbers a wallpaper can
// draw. Whole machine, not this process: the process's own cost is already measured next door in
// GpuMonitor and Application::SampleProcess, and the two are not interchangeable. A wallpaper that
// showed this program's 3% of CPU while the machine sat at 60% would be a gauge that lies.
//
// Every field is -1 until it has been answered, and stays -1 when its source is missing. That is
// inherited from GpuMonitor.hpp's rule and it matters more here: a system monitor drawn from a zero
// is indistinguishable from an idle machine, so "no answer" has to be a value a wallpaper can test
// for and refuse to draw.
//
// Sample() is meant to be called about once a second, not once per frame. PDH's rate counters need a
// window between two collects to answer anything at all, and the first collect of a fresh query
// answers nothing - so a per-frame call would report "no answer" forever. The values it does produce
// move far slower than a frame anyway.
#include "Engine/App/GpuMonitor.hpp"
#include "Engine/Core/Platform.hpp"
#include "Engine/Graphics/D3D11Device.hpp"   // StatsCB

// iphlpapi.h silently leaves MIB_IF_TABLE2, GetIfTable2 and FreeMibTable out unless the socket headers
// have defined the address types they are written in terms of; including it on its own compiles to a
// wall of "undeclared identifier". WIN32_LEAN_AND_MEAN in Platform.hpp already keeps the legacy
// winsock.h out, so winsock2 here is the only one in the translation unit.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <pdh.h>
#include <iphlpapi.h>

namespace sw {

// 32 because that is this machine's logical core count, and the per-core array is the one part of the
// contract whose size a wallpaper has to know at compile time. Beyond coreCount the entries are -1;
// a 64-core host would need the array widened in both this header and Contract.hlsl together.
inline constexpr int kMaxCores = 32;

struct MachineStats {
    // Bit set for each group that answered at least once. A wallpaper uses these to leave a panel out
    // rather than to draw it at zero.
    enum Valid : unsigned {
        kCpu      = 1u << 0,
        kCores    = 1u << 1,
        kMemory   = 1u << 2,
        kGpu      = 1u << 3,
        kDisk     = 1u << 4,
        kNetwork  = 1u << 5,
        kSystem   = 1u << 6,
    };

    float cpuPct = -1.f;        // whole machine, 0..100, from GetSystemTimes
    float cpuPctPdh = -1.f;     // the same quantity from \Processor(_Total)\% Processor Time
    float cpuUserPct = -1.f;    // the same window split the way Task Manager splits it
    float cpuPrivPct = -1.f;
    float cpuQueue = -1.f;
    float core[kMaxCores];
    unsigned coreCount = 0;

    MachineStats() {
        // Not = {}: a zero-filled core array reads as "every core idle", which is the exact failure
        // this header's rule exists to prevent. Absence has to be a value a wallpaper can test for.
        for (float& c : core) c = -1.f;
    }

    float memPct = -1.f, memUsedMb = -1.f, memTotalMb = -1.f, commitMb = -1.f;
    float diskReadBs = -1.f, diskWriteBs = -1.f, diskActivePct = -1.f;
    float diskFreeGb = -1.f, diskTotalGb = -1.f;
    float netRecvBs = -1.f, netSendBs = -1.f;
    float uptimeS = -1.f, processCount = -1.f;

    GpuUse gpu;                 // device numbers only matter here; self is this process, not the machine
    float ageS = 0.f;           // seconds since the sample was taken, so a stalled feed is visible
    unsigned valid = 0;
};

class MachineMetrics {
public:
    // Spelled out rather than left implicit: with the destructor declared and the copies deleted, the
    // message MSVC gives when this one does not resolve names the *containing* member of Application
    // and not the field at fault, which is a slow way to find out.
    MachineMetrics() = default;
    ~MachineMetrics();
    MachineMetrics(const MachineMetrics&) = delete;
    MachineMetrics& operator=(const MachineMetrics&) = delete;

    // gpu is passed in rather than owned: GpuMonitor is the single nvmlInit holder in this process and
    // a second one would have to duplicate the load-and-resolve dance for nothing.
    MachineStats Sample(const GpuUse& gpu);

private:
    bool BuildQuery();
    float Read(PDH_HCOUNTER c, bool* ok = nullptr) const;

    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER cpuTotal_ = nullptr, queue_ = nullptr, procs_ = nullptr;
    PDH_HCOUNTER diskRead_ = nullptr, diskWrite_ = nullptr, diskIdle_ = nullptr;
    std::vector<PDH_HCOUNTER> coreIdle_;
    bool primed_ = false;

    // GetSystemTimes bases, for the whole-machine CPU number. Kept separate from PDH's own total on
    // purpose: two instruments reading one quantity, which is how a wrong counter path gets caught.
    // Plain 64-bit counters rather than ULARGE_INTEGER members: a brace-or-equal initialiser on that
    // union makes MSVC define the class's implicit default constructor as deleted, and the error it
    // reports names the containing member, not this one.
    unsigned long long idle_ = 0, kernel_ = 0, user_ = 0;
    bool haveTimes_ = false;

    unsigned long long netIn_ = 0, netOut_ = 0;
    bool haveNet_ = false;
    double lastNetS_ = 0.0;
    unsigned coreCount_ = 0;
};

// Copy into the constant buffer a wallpaper reads. Field for field with the "Stats" block declared in
// Shaders/Contract.hlsl; a field with no answer is left at the -1 the buffer was constructed with, so
// adding a metric here without adding it there (or the other way round) shifts nothing - it shows up
// as a gap on screen rather than as every later field reading the wrong number.
void FillStatsCB(const MachineStats& x, StatsCB& out);

} // namespace sw
