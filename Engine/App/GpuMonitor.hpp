#pragma once
// Two GPU numbers, because they answer two different questions and are not comparable to each other.
//
//   device  NVML's utilization: the fraction of the last second the card had *any* work in flight.
//           Same number as nvidia-smi, which is what makes it checkable. Loaded at run time rather
//           than linked - nvml.dll comes with the driver, nothing is shipped or built against it, and
//           the four entry points are declared by hand in the .cpp.
//   self    what this process kept an engine busy for, from the same "GPU Engine" performance counters
//           Task Manager's per-process column reads: one percentage per engine we use, added up. On a
//           machine running this wallpaper that is the 3D engine plus the video-decode engine, and the
//           sum can be larger than the device number - different denominators, one card doing two
//           kinds of work at once. Measured 2026-10-08 with a 4K clip on screen: 3D 6.0~8.3% +
//           decode 12.4~16.7% = self 20~27%, while the device read 12~31%. The control that says it
//           tracks the process and not the machine: `--ctl pause` takes self 22.5 -> 0.0 while the
//           device stays at 27~28% for everybody else's work.
//
// PDH is only driven through the enumerate-instances-then-read-each path. Its
// PdhGetFormattedCounterArray was tried first and does not work as documented: the size query leaves
// pdwItemCount undefined, and sizing the buffer from what it reported let it write past the end
// (measured: 58379 items against 664 bytes, then an access violation).
//
// Absence is reported as -1, never as zero: on a machine without NVML a 0 would claim an idle card, and
// a process with no GPU engines has no percentage to add. The same rule carries to the device readings
// below them - a card that will not answer for its temperature reports -1, not 0 C, because 0 C would
// read as "this machine is cold" to anything drawing a gauge from it.
#include <string>
#include <vector>
#include <windows.h>

namespace sw {

struct GpuUse {
    double device = -1.0;
    double self = -1.0;
    double self3d = -1.0;
    double selfDecode = -1.0;
    // The rest of what the card says about itself, for a wallpaper that shows the machine rather than
    // this process. All of it comes from the same nvmlDeviceHandle the two numbers above already use.
    double memUsedMb = -1.0, memTotalMb = -1.0, memReservedMb = -1.0;
    double memUtilPct = -1.0;
    double tempC = -1.0;
    double powerW = -1.0;
    double clockMHz = -1.0;
    std::string name;
};

class GpuMonitor {
public:
    struct Util { unsigned gpu; unsigned memory; };   // nvmlUtilization_t
    struct Memory { unsigned long long total, free_, used; };   // nvmlMemory_t
    // nvmlMemory_v2_t: version-prefixed, and the only one with a field for driver-reserved
    // framebuffer. Asked for first, because v1's `used` answers 2586 MB on this 4080 SUPER while
    // nvidia-smi answers 2256 MB at the same instant - a 330 MB difference, stable across samples,
    // and nvidia-smi's own used+free comes to 16046 against a 16376 total, so the missing 330 is
    // reserved rather than noise. NOTE: on this driver the v2 call does not currently succeed, so the
    // reading in use is still v1's and the HUD's VRAM gauge sits 330 MB above nvidia-smi. Quantified
    // and left open in BACKLOG.md rather than quietly corrected by subtracting a constant.
    struct Memory2 { unsigned version; unsigned long long total, reserved, free_, used; };
    // One "GPU Engine" instance of this process: its name and the counter handle built from it.
    struct Engine { std::wstring name; void* counter = nullptr; };

    GpuMonitor();
    ~GpuMonitor();
    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    // Called from the status builder (once a second from the settings window). The NVML call costs
    // under 0.05 ms; the PDH collect is on the same order. Our engine list is re-enumerated every
    // kEngineRefreshMs because an engine only appears the first time the process uses it.
    GpuUse Sample();

private:
    void RebuildEngines();

    HMODULE lib_ = nullptr;
    void* device_ = nullptr;
    int (*getRates_)(void*, Util*) = nullptr;
    int (*getMem_)(void*, Memory*) = nullptr;
    int (*getMem2_)(void*, Memory2*) = nullptr;
    int (*getTemp_)(void*, unsigned, unsigned*) = nullptr;
    int (*getPower_)(void*, unsigned*) = nullptr;
    int (*getClock_)(void*, unsigned, unsigned*) = nullptr;
    int (*getName_)(void*, char*, unsigned) = nullptr;

    void* query_ = nullptr;
    std::vector<Engine> engines_;
    // The last reading that had a window behind it, and the flag saying the next one does not (a query
    // rebuilt because our engine set changed answers 0.0 for every engine on its first read).
    GpuUse last_;
    bool stale_ = false;
    std::string name_;
};

} // namespace sw
