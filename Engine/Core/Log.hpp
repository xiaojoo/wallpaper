#pragma once
// Engine/Core/Log.hpp - leveled log to file (+ optional console) with std::format.
#include <string>
#include <format>
#include <cstdint>

namespace sw {

enum class Level : int { Trace = 0, Info = 1, Warn = 2, Error = 3 };

namespace log {

bool Open(const std::wstring& file, Level minLevel, bool attachConsole);
void Close();
void SetLevel(Level lvl);
Level Level_();
void Write(Level lvl, const char* module, const std::string& msg);
void Flush();

inline void Log(Level lvl, const char* module, const std::string& msg) { Write(lvl, module, msg); }

} // namespace log

template <class... A> inline void Trace(const char* m, std::format_string<A...> f, A&&... a) {
    log::Write(Level::Trace, m, std::format(std::move(f), std::forward<A>(a)...));
}
template <class... A> inline void Info(const char* m, std::format_string<A...> f, A&&... a) {
    log::Write(Level::Info, m, std::format(std::move(f), std::forward<A>(a)...));
}
template <class... A> inline void Warn(const char* m, std::format_string<A...> f, A&&... a) {
    log::Write(Level::Warn, m, std::format(std::move(f), std::forward<A>(a)...));
}
template <class... A> inline void Error(const char* m, std::format_string<A...> f, A&&... a) {
    log::Write(Level::Error, m, std::format(std::move(f), std::forward<A>(a)...));
}

// Turns an HRESULT into "0x887A0004 (E_FAIL)" style text for logs.
std::string HResultToString(long hr);

} // namespace sw
