#include "Engine/Core/Crash.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace sw {
namespace {

std::wstring gDir;
std::atomic<bool> gInHandler{false};     // one report per process, whatever else falls over

std::string Hex(unsigned long long v, int width) {
    static const char* k = "0123456789ABCDEF";
    std::string out(width, '0');
    for (int i = width - 1; i >= 0; --i) { out[i] = k[v & 0xF]; v >>= 4; }
    return out;
}

// The module a return address belongs to, and its offset inside that module. Asked one handle at a
// time, never enumerated: the toolhelp snapshot allocates, and this runs on a thread that may be
// crashing inside the allocator.
std::string Where(void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod)
        return Hex(reinterpret_cast<unsigned long long>(addr), 16);
    wchar_t path[4096];
    if (!GetModuleFileNameW(mod, path, 4096)) path[0] = L'?', path[1] = 0;
    const std::wstring name = std::wstring(path).substr(std::wstring(path).find_last_of(L"\\/") + 1);
    char utf8[512];
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
    return std::string(utf8) + "+" +
           Hex(reinterpret_cast<unsigned long long>(addr) - reinterpret_cast<unsigned long long>(mod), 6);
}

void WriteAll(HANDLE f, const std::string& s) {
    DWORD n = 0;
    WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
}

// One line per frame of the thread that is going down, plus the exception record. Returns the file
// handle it opened so the two callers can close it the same way.
HANDLE Begin(const char* kind) {
    if (gInHandler.exchange(true)) return nullptr;
    const std::wstring file = gDir + L"\\crash-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char when[64];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    WriteAll(h, std::string(kind) + " " + when + "  pid " + std::to_string(GetCurrentProcessId()) +
                "  tid " + std::to_string(GetCurrentThreadId()) + "\n");
    return h;
}

void Stack(HANDLE h) {
    void* frames[64];
    // Skip 1: this function's own frame. The filter runs on the faulting thread's stack, so what comes
    // back is the call chain that was about to execute the bad instruction.
    const USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
    WriteAll(h, "  stack (" + std::to_string(n) + " frames):\n");
    for (USHORT i = 0; i < n; ++i)
        WriteAll(h, "   " + std::to_string(i) + "  " + Where(frames[i]) + "\n");
}

LONG WINAPI Filter(EXCEPTION_POINTERS* p) {
    HANDLE h = Begin("unhandled exception");
    if (h) {
        const DWORD code = p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionCode : 0;
        void* addr = p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionAddress : nullptr;
        // `at` is the instruction that faulted - the thing to look up in the pdb. The address it was
        // touching is a separate number, and calling the first one "faulting address" reads like the
        // second: an access violation reports which page it reached for, and that is what tells a null
        // `this` apart from a dangling one.
        WriteAll(h, "  code 0x" + Hex(code, 8) + "  at " +
                    (addr ? Where(addr) : std::string("unknown")) + "\n");
        if (code == EXCEPTION_ACCESS_VIOLATION && p->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR kind = p->ExceptionRecord->ExceptionInformation[0];
            const char* verb = kind == 0 ? "reading" : (kind == 1 ? "writing"
                                : (kind == 8 ? "executing" : "touching"));
            WriteAll(h, std::string("  ") + verb + " " +
                        Hex(p->ExceptionRecord->ExceptionInformation[1], 16) + "\n");
        }
        Stack(h);
        CloseHandle(h);
    }
    FlushProcessWriteBuffers();
    return EXCEPTION_EXECUTE_HANDLER;      // die, quietly, with the file left behind
}

[[noreturn]] void OnTerminate() {
    HANDLE h = Begin("std::terminate");
    if (h) {
        // what() is worth one line and is safe enough here: the string already exists, and if the
        // heap is the thing that is broken the stack below is the useful half anyway.
        if (auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                WriteAll(h, std::string("  what: ") + ex.what() + "\n");
            } catch (...) {}
        }
        Stack(h);
        CloseHandle(h);
    }
    TerminateProcess(GetCurrentProcess(), 3);
}

} // namespace

void InstallCrashHandler(const std::wstring& dir) {
    gDir = dir;      // the log directory, which Log::Open has already created
    SetUnhandledExceptionFilter(Filter);
    std::set_terminate(OnTerminate);
}

} // namespace sw
