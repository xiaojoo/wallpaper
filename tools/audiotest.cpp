// tools/audiotest.cpp - a standalone WASAPI tone with a volume slider and the one number the wallpaper
// engine has never read: how many frames the device says it has actually PLAYED (IAudioClient::
// GetPosition). Everything else in this repo's audio path reports what we handed over; this reports
// what came out the other end.
//
// Build: tools/audiotest-build.bat -> build/audiotest.exe. Run it, drag the slider.
//
// Why each field is on screen:
//   设备 / 端点音量   - the endpoint we opened and its own master level. "扬声器 (X)" that is not the
//                       thing you are listening on explains silence without any bug in the code.
//   本程序会话         - this process's slot in the Windows mixer, read through IAudioSessionManager2.
//                       Our plugin cannot get it (GetService returns E_NOINTERFACE), so this program
//                       asks the same question a different way and shows the answer either way.
//   period / bufFrames - the buffer we agreed with the device. This program passes duration 0, which
//                       means "use the device's own default period"; the wallpaper asks for 100 ms.
//                       If this program is audible and the wallpaper is not, that difference is the
//                       first thing to copy across.
//   播放帧数 / 速率     - GetPosition's play cursor converted to frames, and its rate over the last
//                       half second. 48 000/s means the audio engine really rendered what we wrote.
//                       0 or a crawl means it took our buffers and never played them - which is what
//                       "ready, playing, underruns 0, 48 000 frames/s written" cannot tell you.
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propsys.h>

#include "../Engine/Audio/IAudioPlugin.h"

#include <atomic>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

const PROPERTYKEY kFriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

HWND g_win = nullptr, g_status = nullptr, g_slider = nullptr, g_button = nullptr;
std::atomic<bool> running{true};
std::atomic<int> gainPct{100};
std::atomic<unsigned long long> played{0};       // frames the device reports as played
std::atomic<unsigned long long> written{0};      // frames we handed over
std::atomic<unsigned> paddingNow{0};
std::string g_device, g_endpoint, g_session;
unsigned g_rate = 48000, g_ch = 2, g_buf = 0;
double g_periodMs = 0.0;
// --plugin: drive the wallpaper's own backend DLL from THIS process. This program is audible on this
// machine, so the two possible answers split the remaining space in one run: audible here means the
// engine's process is what differs, silence here means the client the DLL builds is what differs.
bool g_usePlugin = false;
std::string g_pluginLine;

std::string W8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void ReadSessionSlot() {
    // The mixer slot for this process. IAudioClient::GetService refuses IAudioSessionManager here (it
    // is what our plugin hit: 0x80004002), so go through the policy factory instead - that is the
    // supported way to get a session manager that is not tied to one client.
    CoTaskMemFree(nullptr);
    IAudioSessionManager2* sm = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        g_session = "枚举器打不开";
        return;
    }
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) {
        en->Release();
        g_session = "没有默认端点";
        return;
    }
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&sm)) || !sm) {
        g_session = "端点上取不到 IAudioSessionManager2";
    } else {
        IAudioSessionEnumerator* se = nullptr;
        if (SUCCEEDED(sm->GetSessionEnumerator(&se)) && se) {
            int n = 0;
            se->GetCount(&n);
            const DWORD mine = GetCurrentProcessId();
            bool found = false;
            char buf[128];
            for (int i = 0; i < n && !found; ++i) {
                IAudioSessionControl* sc = nullptr;
                if (FAILED(se->GetSession(i, &sc)) || !sc) continue;
                IAudioSessionControl2* sc2 = nullptr;
                if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                    DWORD pid = 0;
                    sc2->GetProcessId(&pid);
                    if (pid == mine) {
                        ISimpleAudioVolume* sav = nullptr;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sav)) && sav) {
                            float v = -1.f;
                            BOOL m = FALSE;
                            sav->GetMasterVolume(&v);
                            sav->GetMute(&m);
                            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "会话 %d%%%s",
                                        (int)(v * 100.f + 0.5f), m ? " 静音" : "");
                            g_session = buf;
                            sav->Release();
                        } else g_session = "会话没有音量接口";
                        found = true;
                    }
                    sc2->Release();
                }
                sc->Release();
            }
            if (!found) g_session = "混音器里还没有本程序的格子";
            se->Release();
        } else g_session = "会话枚举器打不开";
        sm->Release();
    }
    dev->Release();
    en->Release();
}

void ReadEndpoint(IMMDevice* dev) {
    IPropertyStore* st = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st)) && st) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(st->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR) g_device = W8(pv.pwszVal);
        PropVariantClear(&pv);
        st->Release();
    }
    IAudioEndpointVolume* epv = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&epv)) && epv) {
        float v = -1.f;
        BOOL m = FALSE;
        epv->GetMasterVolumeLevelScalar(&v);
        epv->GetMute(&m);
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "端点 %d%%%s", (int)(v * 100.f + 0.5f), m ? " 静音" : "");
        g_endpoint = buf;
        epv->Release();
    } else g_endpoint = "端点音量读不到";
}

DWORD WINAPI AudioThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) return 1;
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { en->Release(); return 1; }
    en->Release();
    ReadEndpoint(dev);

    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) {
        g_endpoint += " / IAudioClient 激活失败";
        dev->Release();
        return 1;
    }
    dev->Release();

    WAVEFORMATEX* wf = nullptr;
    ac->GetMixFormat(&wf);
    if (wf) { g_rate = wf->nSamplesPerSec; g_ch = wf->nChannels; }
    // duration 0 = the device's own default period, which is the shape a normal player uses.
    const HRESULT hinit = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, wf, nullptr);
    if (wf) CoTaskMemFree(wf);
    if (FAILED(hinit)) {
        char b[64];
        _snprintf_s(b, sizeof(b), _TRUNCATE, " / Initialize 失败 hr=0x%08lX", (unsigned long)hinit);
        g_endpoint += b;
        return 1;
    }
    UINT32 bf = 0;
    ac->GetBufferSize(&bf);
    g_buf = bf;
    g_periodMs = 1000.0 * (double)bf / (double)g_rate;

    IAudioRenderClient* rc = nullptr;
    if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) return 1;
    // The played-frame counter is not on IAudioClient - it is the audio clock's device position, in
    // the clock's own units (100 ns at the documented frequency). This is the number that says the
    // audio engine really rendered what we wrote, rather than merely accepting it.
    IAudioClock* clock = nullptr;
    ac->GetService(__uuidof(IAudioClock), (void**)&clock);
    unsigned long long clockFreq = 10000000ull;
    if (clock) {
        UINT64 f = 0;
        if (SUCCEEDED(clock->GetFrequency(&f)) && f) clockFreq = (unsigned long long)f;
    }
    ReadSessionSlot();
    ac->Start();

    double phase = 0.0;
    while (running.load()) {
        Sleep(10);
        UINT32 pad = 0;
        if (FAILED(ac->GetCurrentPadding(&pad))) break;
        paddingNow.store(pad);
        const UINT32 space = bf > pad ? bf - pad : 0;
        if (space) {
            BYTE* d = nullptr;
            if (SUCCEEDED(rc->GetBuffer(space, &d))) {
                const float g = gainPct.load() / 100.f;
                float* f = reinterpret_cast<float*>(d);
                for (UINT32 i = 0; i < space; ++i) {
                    const float v = g * (float)std::sin(phase * 6.283185307179586);
                    for (UINT32 c = 0; c < g_ch; ++c) f[i * g_ch + c] = v;
                    phase += 440.0 / (double)g_rate;
                    if (phase >= 1.0) phase -= 1.0;
                }
                rc->ReleaseBuffer(space, 0);
                written += space;
            }
        }
        UINT64 pos = 0;
        UINT64 drop = 0;
        if (clock && SUCCEEDED(clock->GetPosition(&pos, &drop))) {
            const unsigned long long frames =
                (unsigned long long)((double)pos * (double)g_rate / (double)clockFreq);
            played.store(frames);
        }
    }
    ac->Stop();
    rc->Release();
    ac->Release();
    CoUninitialize();
    return 0;
}

// The same tone, through the wallpaper's backend: LoadLibrary the DLL the engine loads, take its
// function table, open, and feed it float32 at the rate it reports. Nothing here is ours except the
// ring behind `write`.
DWORD WINAPI PluginThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const wchar_t* dll = L"H:\\wallpaper\\bld\\bin\\RelWithDebInfo\\swaudio_wasapi.dll";
    HMODULE lib = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    DWORD e1 = lib ? 0 : GetLastError();
    if (!lib) lib = LoadLibraryW(dll);          // plain search, to tell "missing file" from "missing flag"
    if (!lib) {
        char b[160];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "LoadLibrary 失败 带标志 err=%lu / 普通 err=%lu",
                    e1, GetLastError());
        g_pluginLine = b;
        return 1;
    }
    auto get = reinterpret_cast<const SwAudioPlugin* (*)()>(GetProcAddress(lib, "swaudio_get"));
    if (!get) { g_pluginLine = "没有 swaudio_get 导出"; return 1; }
    const SwAudioPlugin* p = get();
    if (!p || p->abi != SWAUDIO_ABI) {
        char b[128];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "abi %u, 本程序要 %u", p ? p->abi : 0u, SWAUDIO_ABI);
        g_pluginLine = b;
        return 1;
    }
    unsigned rate = 0, ch = 0;
    char err[320] = "";
    if (p->open(&rate, &ch, err, sizeof(err)) != 0) {
        g_pluginLine = std::string("open 失败: ") + err;
        return 1;
    }
    g_rate = rate ? rate : 48000;
    g_ch = ch ? ch : 2;
    g_device = "经 swaudio_wasapi.dll";
    g_endpoint = err;              // on success the backend puts the route here
    g_session = "见上一行";
    g_periodMs = p->latencyMs ? (double)p->latencyMs() : 0.0;
    g_buf = (unsigned)(g_periodMs * g_rate / 1000.0);
    p->setVolume(1.0f);
    p->setPaused(0);

    std::vector<float> blk(1024u * g_ch, 0.f);
    double phase = 0.0;
    while (running.load()) {
        const float g = gainPct.load() / 100.f;
        for (unsigned i = 0; i < 1024; ++i) {
            const float v = g * (float)std::sin(phase * 6.283185307179586);
            for (unsigned c = 0; c < g_ch; ++c) blk[(size_t)i * g_ch + c] = v;
            phase += 440.0 / (double)g_rate;
            if (phase >= 1.0) phase -= 1.0;
        }
        unsigned off = 0;
        while (off < 1024 && running.load()) {
            const int took = p->write(blk.data() + (size_t)off * g_ch, 1024 - off);
            if (took <= 0) { Sleep(5); continue; }
            off += (unsigned)took;
            written += (unsigned long long)took;
        }
        if (p->route) {
            char r[320] = "";
            p->route(r, sizeof(r));
            g_pluginLine = r;
            const char* q = strstr(r, "played=");
            if (q) played.store(strtoull(q + 7, nullptr, 10));
        }
        Sleep(5);
    }
    p->setPaused(1);
    p->close();
    CoUninitialize();
    return 0;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HSCROLL && (HWND)l == g_slider) {
        gainPct.store((int)SendMessageW(g_slider, TBM_GETPOS, 0, 0));
        return 0;
    }
    if (m == WM_COMMAND && (HWND)l == g_button) {
        running.store(!running.load());
        SetWindowTextW(g_button, running.load() ? L"停止" : L"播放");
        return 0;
    }
    if (m == WM_TIMER) {
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "%s\n设备        %s\n%s\n%s\n"
                    "缓冲        period %.1f ms  (%u 帧  =  我们自定 100 ms 时是 4800)\n"
                    "已写入      %llu 帧\n设备已播放  %llu 帧\n滑块        %d%%  (线性振幅，100%% = 满幅 440 Hz)",
                    g_usePlugin ? "模式：经 swaudio_wasapi.dll（壁纸那份后端）" : "模式：本程序自己的 WASAPI 客户端",
                    g_device.empty() ? "还没打开设备" : g_device.c_str(),
                    g_endpoint.empty() ? "端点音量: -" : g_endpoint.c_str(),
                    g_pluginLine.empty() ? (g_session.empty() ? "本程序会话: -" : g_session.c_str())
                                         : g_pluginLine.c_str(),
                    g_periodMs, g_buf, written.load(), played.load(), gainPct.load());
        const int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
        static wchar_t wide[768];
        if (n > 0 && n <= 768) MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, n);
        SetWindowTextW(g_status, wide);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"plugin")) g_usePlugin = true;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SwAudioTest";
    RegisterClassW(&wc);

    g_win = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"音频测试 - WASAPI 共享模式（独立程序，不经过壁纸引擎）",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            120, 120, 560, 330, nullptr, nullptr, hi, nullptr);
    g_status = CreateWindowExW(0, L"STATIC", L"正在打开设备…", WS_CHILD | WS_VISIBLE,
                               14, 10, 520, 150, g_win, nullptr, hi, nullptr);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)f, TRUE);
    g_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                               14, 175, 420, 32, g_win, nullptr, hi, nullptr);
    SendMessageW(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(g_slider, TBM_SETPOS, TRUE, gainPct.load());
    g_button = CreateWindowExW(0, L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               444, 173, 90, 34, g_win, nullptr, hi, nullptr);
    CreateWindowExW(0, L"STATIC", L"提示：拖动滑块听音调；「设备已播放」若每秒 +48000 而房间没声音，锅在端点/驱动，不在这段代码。",
                    WS_CHILD | WS_VISIBLE, 14, 222, 520, 40, g_win, nullptr, hi, nullptr);

    ShowWindow(g_win, SW_SHOW);
    UpdateWindow(g_win);
    SetTimer(g_win, 1, 250, nullptr);

    HANDLE t = CreateThread(nullptr, 0, g_usePlugin ? PluginThread : AudioThread, nullptr, 0, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    running.store(false);
    if (t) { WaitForSingleObject(t, 1000); CloseHandle(t); }
    CoUninitialize();
    return 0;
}
