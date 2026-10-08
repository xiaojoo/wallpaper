// main.cpp - entry point: either run the renderer or act as the control client (--ctl).
#include "Engine/App/Application.hpp"
#include "Engine/App/IPCServer.hpp"
#include "Engine/Core/Log.hpp"

#include <shellapi.h>
#include <cstdio>

using namespace sw;

namespace {

std::wstring ExeDir() {
    wchar_t buf[4096];
    GetModuleFileNameW(nullptr, buf, 4095);
    std::wstring path = buf;
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

void AppendStartupLog(const std::string& text) {
    // The failure paths run before log::Open, and the settings window relaunches this exe detached
    // with no console attached, so there is nowhere else the message could go.
    std::wstring dir = ExeDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\startup-error.log").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " " + text;
    DWORD written = 0;
    WriteFile(h, line.data(), DWORD(line.size()), &written, nullptr);
    CloseHandle(h);
}

void PrintToParentConsole(const std::string& text) {
    static bool attached = [] {
        if (GetConsoleWindow()) return true;
        return AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    }();
    if (attached) {
        SetConsoleOutputCP(CP_UTF8);
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    } else {
        // Never a modal dialog: he asked for this process to just exit when it cannot do its job,
        // and a message box sits on top of the desktop he is working on.
        OutputDebugStringA(text.c_str());
        AppendStartupLog(text);
    }
}

std::string QuoteJson(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if ((unsigned char)c < 0x20) { /* drop */ }
        else out.push_back(c);
    }
    return out;
}

void Usage() {
    PrintToParentConsole(
        "Wallpaper renderer\n"
        "\n"
        "  WallpaperRenderer.exe                      start rendering (reads config.json)\n"
        "  WallpaperRenderer.exe --selftest 20        run 20 s, then exit and log the measurements\n"
        "  WallpaperRenderer.exe --wallpaper aurora   start showing one wallpaper on every monitor\n"
        "  WallpaperRenderer.exe --quality high       override all wallpapers with a quality level\n"
        "  WallpaperRenderer.exe --fps 30             global frame cap\n"
        "  WallpaperRenderer.exe --window             normal top-level windows, desktop untouched\n"
        "  WallpaperRenderer.exe --no-tray --no-ipc   run without tray icon / control pipe\n"
        "  WallpaperRenderer.exe --log trace\n"
        "\n"
        "Control a running renderer:\n"
        "  WallpaperRenderer.exe --ctl status\n"
        "  WallpaperRenderer.exe --ctl list\n"
        "  WallpaperRenderer.exe --ctl apply aurora [M0|all]\n"
        "  WallpaperRenderer.exe --ctl pause | resume | reload | quit\n"
        "  WallpaperRenderer.exe --ctl quality battery\n"
        "  WallpaperRenderer.exe --ctl fps 30\n"
        "  WallpaperRenderer.exe --ctl taskbar 40\n");
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // A crash in this process has to be a crash and nothing else. With the default error mode an
    // unhandled access violation puts Windows' "WallpaperRenderer.exe has stopped working" dialog on
    // top of the desktop he is working on: the screen waits on it, and the wallpaper is black behind
    // it until someone dismisses it. Silencing the prompt does not hide a fault - the process still
    // dies, still leaves its log, and the settings window now brings the engine back.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    CommandLine cl;
    cl.exeDir = ExeDir();

    auto next = [&](int& i) -> std::string { return i + 1 < argc ? ToUtf8(argv[++i]) : std::string(); };

    bool ctl = false;
    std::string ctlCmd, ctlArg, ctlArg2;
    std::string pipeOverride;

    for (int i = 1; i < argc; ++i) {
        std::string a = ToUtf8(argv[i]);
        if (a == "--selftest") cl.selftestSeconds = std::max(1, std::atoi(next(i).c_str()));
        else if (a == "--wallpaper") cl.applyWallpaper = next(i);
        else if (a == "--quality") cl.qualityOverride = next(i);
        else if (a == "--fps") cl.fpsOverride = std::atoi(next(i).c_str());
        else if (a == "--log") cl.logLevel = next(i);
        else if (a == "--pipe") pipeOverride = next(i);
        else if (a == "--no-tray") cl.noTray = true;
        else if (a == "--no-ipc") cl.noIpc = true;
        else if (a == "--no-power") cl.noPower = true;
        else if (a == "--no-desktop" || a == "--window") cl.noDesktop = true, cl.logToConsole = true;
        else if (a == "--console") cl.logToConsole = true;
        else if (a == "--ctl") {
            ctl = true;
            ctlCmd = next(i);
            ctlArg = next(i);
            ctlArg2 = next(i);
        } else if (a == "--help" || a == "-h" || a == "/?") {
            Usage();
            return 0;
        } else {
            PrintToParentConsole("unknown argument: " + a + "\n");
            Usage();
            return 2;
        }
    }
    cl.pipeOverride = pipeOverride;

    if (ctl) {
        std::wstring pipe = pipeOverride.empty()
                                ? L"\\\\.\\pipe\\SmartWallpaper.Renderer"
                                : ToWide(pipeOverride);
        if (ctlCmd.empty()) {
            PrintToParentConsole("--ctl needs a command: status|list|apply|pause|resume|quality|fit|fps|taskbar|reload|quit\n");
            return 2;
        }
        Json q = Json::Object();
        q.set("cmd", Json::Of(ctlCmd));
        if (!ctlArg.empty()) q.set("arg", Json::Of(ctlArg));
        if (!ctlArg2.empty()) q.set("arg2", Json::Of(ctlArg2));
        IpcReply r = IPCCall(pipe, q.dump(), cl.selftestSeconds ? 5000 : 4000);
        if (!r.connected) {
            PrintToParentConsole("cannot reach the renderer: " + r.error + "\n");
            return 3;
        }
        PrintToParentConsole(r.response + "\n");
        return 0;
    }

    Application app;
    std::string error;
    if (!app.Start(cl, error)) {
        std::string text = "Wallpaper could not start: " + error +
                           "\nSee logs/renderer-<pid>.log for the full trace.";
        PrintToParentConsole(text + "\n");
        return 1;
    }
    int rc = app.Run();
    app.Shutdown();
    return rc;
}
