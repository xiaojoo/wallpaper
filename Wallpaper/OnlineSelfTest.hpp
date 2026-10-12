#pragma once
// Wallpaper/OnlineSelfTest.hpp - a headless end-to-end check of the online page, behind
// `Wallpaper.exe --online-selftest <query>`.
//
// It drives the real Bridge rather than a parallel implementation, so what it reports is what the
// page does: the source description parsed, the search answered, a picture downloaded, the renderer
// imported it, the catalog read it back - and then it deletes what it imported, so the library ends
// where it started. This is a GUI-subsystem exe with no console, so the report goes to a file the
// caller names.
#include <QString>

namespace sw {

class Bridge;

int RunOnlineSelfTest(Bridge& bridge, const QString& query, const QString& outFile,
                      const QString& source = {});

} // namespace sw
