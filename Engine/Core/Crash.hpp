#pragma once
// Engine/Core/Crash.hpp - make the engine say where it died, because nothing else on this machine will.
//
// main.cpp sets SEM_NOGPFAULTERRORBOX on purpose: an unhandled access violation must not put Windows'
// "WallpaperRenderer.exe has stopped working" dialog on top of the desktop he is working in, with the
// wallpaper black behind it until someone clicks it. The cost nobody looked at is that the same switch
// (together with no WER local-dump configuration) means Windows writes no fault report either. So an
// access violation leaves exactly one trace: a log that stops mid-line. That is what happened on
// 2026-10-11 - the settings window's parent read the child's exit code and got 0xC0000005, and the
// engine's own log had nothing to say about where.
//
// This installs a filter that writes the exception code, the faulting address and the calling stack as
// `module + offset` to logs/crash-<pid>.txt before the process dies. Module and offset is deliberately
// not symbolised in the crashing process: resolving symbols there would mean loading dbghelp, which
// allocates and reads the pdb from disk, on a thread that may be crashing inside the heap. The offset
// is enough to resolve offline against the .pdb that sits next to the exe.
//
// The same handler covers `std::terminate`, which is how a `std::system_error` escaping a worker
// thread's destructor ends this process today (see the note in FFmpegPlayer about the thread join).
//
// Turning a line of that file back into a source location, with the .pdb that sits next to the exe:
//   cdb -c "ln WallpaperRenderer.exe+29298; q" <bin>\WallpaperRenderer.exe
// The offsets are stable for a given build, so the crash file and the exe it came from are enough -
// which is why nothing is symbolised in the crashing process itself.
// tools/probe-crash-selftest.ps1 proves the whole thing fires, via the engine's `--selfcrash` switch.
#include "Engine/Core/Platform.hpp"

namespace sw {

// `dir` is where logs go; the file is created there as crash-<pid>.txt. Call once, before anything can
// throw. A missing or unwritable directory is not reported - there is no safe channel to report it on
// the path that needs this.
void InstallCrashHandler(const std::wstring& dir);

} // namespace sw
