#pragma once
// Engine/System/DiskTemp.hpp - the temperature of the disk the wallpapers live on.
//
// This is the one temperature on a Windows desktop that has no performance counter and no WMI class
// an ordinary process can read: the StorageWMI provider that `Get-StorageReliabilityCounter` goes
// through wants elevation, and the SATA SMART relay wants it too. NVMe drives answer it directly
// through IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceTemperatureProperty, which the in-box
// storNVMe driver serves to an unelevated caller - measured on this machine at 53 C and 42 C on two
// drives, with the drive's own warning and critical thresholds alongside.
//
// So the property id matters more than it looks: 43 returns ERROR_INVALID_FUNCTION, which is
// indistinguishable from "this machine cannot answer" unless you count the enum from winioctl.h.
// StorageDeviceTemperatureProperty is 52.
//
// The disk is chosen by following the wallpaper root's own volume down to the number the volume sits
// on, not by taking PhysicalDrive0: this machine has two NVMe drives and the one that matters is the
// one holding the files being read.

#include <string>

namespace sw {

struct DiskTemp {
    float tempC = -1.f;      // the composite sensor, the one that means "how hot is this drive"
    float warnC = -1.f;      // the drive's own warning threshold
    float critC = -1.f;      // the drive's own critical threshold
    int sensors = 0;
    std::wstring path;       // the disk object it came from, for the log
    std::string why;         // where the chain broke, empty before it is tried
};

// Reads the disk under `rootPath` (a directory like "C:\Wallpaper"). Returns all -1 when the volume
// cannot be mapped, the disk cannot be opened, or the drive does not answer - which on a SATA or a
// USB disk is normal, and is why the field is -1 rather than 0: a gauge that draws an unanswered
// temperature as 0 C claims a cold drive.
DiskTemp ReadDiskTemp(const std::wstring& rootPath);

} // namespace sw
