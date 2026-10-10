#include "Engine/System/DiskTemp.hpp"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <string>

namespace sw {
namespace {

// The property id, spelled out rather than inlined at the call: 43 answers ERROR_INVALID_FUNCTION and
// reads exactly like a machine that cannot report a temperature. See the header.
constexpr int kStorageDeviceTemperatureProperty = 52;
constexpr int kStorageStandardProperty = 0;
constexpr short kNotReported = (short)0x8000;

// A handle that closes itself, because there are three of them here and one early return each.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { Close(); }
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    void Close() { if (h_ != INVALID_HANDLE_VALUE && h_) CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
private:
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool QueryProperty(HANDLE h, int propertyId, BYTE* out, DWORD outSize) {
    // 28 bytes, not 8: STORAGE_PROPERTY_QUERY is declared with a trailing AdditionalParameters[1],
    // and storNVMe answers ERROR_BAD_LENGTH (24) for an input buffer that is only the two DWORDs.
    // The first 8 bytes are the property id and PropertyStandardQuery; the rest is the empty tail.
    BYTE in[28]{};
    DWORD pid = (DWORD)propertyId;
    memcpy(in, &pid, sizeof(pid));
    DWORD ret = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, in, sizeof(in),
                           out, outSize, &ret, nullptr) && ret > 0;
}

// The disk number the volume behind `rootPath` lives on. -1 when the chain breaks anywhere.
int DiskNumberFor(const std::wstring& rootPath, std::string* why) {
    if (rootPath.size() < 2 || rootPath[1] != L':') { if (why) *why = "root is not a drive path"; return -1; }
    // No trailing separator: "\.\H:\" answers ERROR_INVALID_HANDLE on the extents IOCTL while
    // "\.\H:" answers the disk number - both measured on this machine, same access.
    std::wstring vol = L"\\\\.\\" + rootPath.substr(0, 2);
    Handle h(CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
    if (!h) { if (why) *why = "volume open failed " + std::to_string((int)GetLastError()); return -1; }
    // VOLUME_DISK_EXTENTS is variable-length; 4 KB covers a volume spread over far more disks than
    // any desktop has.
    BYTE buf[4096]{};
    DWORD ret = 0;
    if (!DeviceIoControl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                         buf, sizeof(buf), &ret, nullptr))
        { if (why) *why = "extents failed " + std::to_string((int)GetLastError()); return -1; }
    auto* ex = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
    if (ex->NumberOfDiskExtents < 1) { if (why) *why = "no extents"; return -1; }
    return (int)ex->Extents[0].DiskNumber;
}

} // namespace

DiskTemp ReadDiskTemp(const std::wstring& rootPath) {
    DiskTemp out;
    std::string why;
    int number = DiskNumberFor(rootPath, &why);
    if (number < 0) number = 0;      // a single-disk machine with an unmappable volume still has a 0
    std::wstring dev = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    out.path = dev;
    Handle h(CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
    if (!h) { out.why = why + ", disk open failed " + std::to_string((int)GetLastError()); return out; }

    BYTE buf[512]{};
    if (!QueryProperty(h.get(), kStorageDeviceTemperatureProperty, buf, sizeof(buf))) {
        out.why = why + ", temperature query failed " + std::to_string((int)GetLastError());
        return out;
    }
    auto* d = reinterpret_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    if (d->CriticalTemperature != kNotReported) out.critC = (float)d->CriticalTemperature;
    if (d->WarningTemperature != kNotReported) out.warnC = (float)d->WarningTemperature;
    out.sensors = (int)d->InfoCount;
    // Index 0 is documented as "may indicate a composite value", which is the one number that means
    // the drive as a whole. The rest are per-sensor and are not what a desktop readout wants.
    for (int i = 0; i < d->InfoCount && i < 8; ++i) {
        const STORAGE_TEMPERATURE_INFO& t = d->TemperatureInfo[i];
        if (t.Index != 0) continue;
        if (t.Temperature != kNotReported) out.tempC = (float)t.Temperature;
        break;
    }
    out.why = why.empty() ? "ok" : why;
    if (out.tempC < 0.f) {
        // No composite sensor: fall back to the hottest one reported, because for "is this drive
        // getting hot" the maximum is the honest answer and the first index is not necessarily it.
        float best = -1.f;
        for (int i = 0; i < d->InfoCount && i < 8; ++i) {
            const STORAGE_TEMPERATURE_INFO& t = d->TemperatureInfo[i];
            if (t.Temperature != kNotReported && (float)t.Temperature > best) best = (float)t.Temperature;
        }
        out.tempC = best;
    }
    return out;
}

} // namespace sw
