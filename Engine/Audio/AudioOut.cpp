#include "Engine/Audio/AudioOut.hpp"
#include "Engine/Core/Log.hpp"

#include <windows.h>

#include <cwchar>

namespace sw {
namespace {

// The file the engine looks for, next to itself. A name rather than a search path on purpose: the
// plugin is chosen by the build, and a stray DLL found through PATH is not something a wallpaper
// should be able to pull in.
constexpr const wchar_t* kPluginFile = L"swaudio_wasapi.dll";
constexpr const char* kPluginName = "swaudio_wasapi.dll";   // for the narrow strings the log carries

std::wstring ExeDir() {
    wchar_t buf[4096] = {};
    if (!GetModuleFileNameW(nullptr, buf, 4095)) return {};
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

} // namespace

AudioOut::~AudioOut() { Close(); }

bool AudioOut::Open(std::string& note) {
    if (p_) { note = "already open"; return true; }
    const std::wstring dir = ExeDir();
    if (dir.empty()) { note = "no module path"; return false; }
    const std::wstring path = dir + L"\\" + kPluginFile;

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so the backend's own dependencies resolve next to it, in the
    // engine's directory, and not from whatever happens to be on PATH.
    lib_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!lib_) {
        note = std::string("no audio backend (") + kPluginName
             + " missing, err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto get = reinterpret_cast<const SwAudioPlugin* (*)()>(
        GetProcAddress(static_cast<HMODULE>(lib_), "swaudio_get"));
    if (!get) { note = "backend exports no swaudio_get"; Close(); return false; }
    const SwAudioPlugin* p = get();
    if (!p || p->abi != SWAUDIO_ABI) {
        note = "backend ABI " + std::to_string(p ? p->abi : 0u) + ", engine wants "
             + std::to_string(SWAUDIO_ABI);
        Close();
        return false;
    }
    p_ = p;

    char err[320] = {};
    if (p_->open(&rate_, &ch_, err, sizeof(err)) != 0) {
        note = std::string(p_->name ? p_->name : "backend") + " open failed: " + err;
        p_ = nullptr;
        Close();
        return false;
    }
    note = std::string(p_->name ? p_->name : "backend") + " "
         + std::to_string(rate_) + " Hz x " + std::to_string(ch_)
         + ", buffer " + std::to_string(latencyMs()) + " ms";
    // On success the plugin's `err` buffer carries the route (endpoint name, its level, our own
    // per-app level) rather than an error, and "why is it silent" is answered by this one line.
    if (err[0]) note += ", " + std::string(err);
    Info("audio", "{}", note);
    return true;
}

void AudioOut::Close() {
    if (p_ && p_->close) p_->close();
    if (lib_) { FreeLibrary(static_cast<HMODULE>(lib_)); lib_ = nullptr; }
    p_ = nullptr;
    rate_ = ch_ = 0;
}

} // namespace sw
