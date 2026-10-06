#include "Log.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <chrono>

namespace sw::log {
namespace {

std::mutex g_mtx;
std::ofstream g_file;
Level g_level = Level::Info;
bool g_console = false;
bool g_open = false;

const char* Tag(Level l) {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Info:  return "info ";
        case Level::Warn:  return "warn ";
        default:           return "error";
    }
}

std::string Stamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms);
    return buf;
}

} // namespace

bool Open(const std::wstring& file, Level minLevel, bool attachConsole) {
    std::lock_guard lk(g_mtx);
    g_level = minLevel;
    g_console = attachConsole;
    if (g_open) g_file.close();
    g_file.open(file, std::ios::out | std::ios::app);
    g_open = static_cast<bool>(g_file);
    if (attachConsole) {
        if (!GetConsoleWindow()) AllocConsole();
        SetConsoleOutputCP(CP_UTF8);
        ShowWindow(GetConsoleWindow(), SW_SHOW);
    }
    return g_open;
}

void Close() {
    std::lock_guard lk(g_mtx);
    if (g_open) g_file.close();
    g_open = false;
}

void SetLevel(Level lvl) { std::lock_guard lk(g_mtx); g_level = lvl; }
Level Level_() { std::lock_guard lk(g_mtx); return g_level; }

void Write(Level lvl, const char* module, const std::string& msg) {
    std::lock_guard lk(g_mtx);
    if (lvl < g_level) return;
    std::string line = Stamp();
    line += ' ';
    line += Tag(lvl);
    line += " [";
    line += module ? module : "-";
    line += "] ";
    line += msg;
    line += '\n';
    if (g_open) { g_file << line; g_file.flush(); }
    if (g_console) std::fputs(line.c_str(), stdout);
    OutputDebugStringA(line.c_str());
}

void Flush() { std::lock_guard lk(g_mtx); if (g_open) g_file.flush(); }

} // namespace sw::log

namespace sw {

std::string HResultToString(long hr) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)hr, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string text;
    if (n && buf) {
        text.assign(buf, n);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        LocalFree(buf);
    }
    char hex[24];
    std::snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)hr);
    return text.empty() ? std::string(hex) : std::string(hex) + " " + text;
}

} // namespace sw
