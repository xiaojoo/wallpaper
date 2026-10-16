// Engine/Audio/WasapiPlugin.cpp - the built-in backend: WASAPI shared mode on the default endpoint.
//
// Built as a DLL and loaded at run time by Engine/Audio/AudioOut.cpp, so it is a component that can
// be taken away rather than a dependency the engine cannot start without. Shared mode is not a
// simplification, it is the requirement: a wallpaper must not take the output device exclusively,
// because that silences whatever else the machine is playing, and on a machine with a call or a game
// already up the exclusive open fails outright.
//
// Threading: one producer (the engine's decoder thread, through `write`) and one consumer (the render
// thread, which the device pokes). They meet in a lock-free ring of frames - single producer, single
// consumer, power-of-two size, indices only ever compared as 64-bit frame counts - so no lock sits on
// the audio path.
#include "Engine/Audio/IAudioPlugin.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <audiopolicy.h>   // IAudioSessionManager / IAudioSessionControl2 live here, not in audioclient.h
#include <propsys.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace {

constexpr unsigned long long kRingFrames = 1ull << 16;   // 65536 frames: ~1.4 s at 48 kHz
constexpr unsigned long long kRingMask   = kRingFrames - 1;
// 0 = take the endpoint's own default period. This used to ask for 100 ms (GetBufferSize then handed
// back 4800 frames), and every counter said the stream was healthy - written at 48 000 frames/s, zero
// underruns, post-gain peak 1000/1000 - while the room heard nothing. A standalone probe that passes
// 0 is audible down to 20 % on the same endpoint in the same session, so the device's own period is
// now the only thing the two clients have in common.
constexpr REFERENCE_TIME kBufferHns = 0;

// PKEY_Device_FriendlyName. Written out rather than included: the header that carries it
// (functiondiscoverykeys_devpnp.h) is not in this SDK's include path, and a wallpaper has no business
// depending on a property-key header to say which speakers it picked.
const PROPERTYKEY kPKeyDeviceFriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

struct State {
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    // The device's own clock. `GetPosition` on it is how many frames the audio engine says it has
    // PLAYED - the only number in this chain that is not our own bookkeeping, and the one that
    // separates "the device took 48 000 frames a second" from "the device played them".
    IAudioClock* clock = nullptr;
    HANDLE thread = nullptr;

    std::vector<float> ring;                              // kRingFrames * ch, interleaved float32
    std::atomic<unsigned long long> wpos{0};              // frames produced
    std::atomic<unsigned long long> rpos{0};              // frames consumed

    unsigned rate = 0, ch = 0, bufFrames = 0;
    unsigned bytesPerFrame = 4;
    bool pcm16 = false;

    std::atomic<float> volume{0.0f};       // what the engine asked for
    std::atomic<float> volumeNow{0.0f};    // what is being applied right now, ramped toward the above
    std::atomic<long> paused{1};
    std::atomic<long> quit{0};
    std::atomic<unsigned> underruns{0};
    std::atomic<unsigned> latencyMs{0};
    // The loudest sample this backend actually put on the wire, post-gain, in permille of full scale,
    // decaying. It exists because "everything reads working but nothing is audible" has exactly two
    // answers - we are writing silence, or we are not the ones making sound - and only the samples
    // themselves tell them apart.
    std::atomic<unsigned> peak{0};
    // Raw bytes as handed to the device, dumped to the file named by SWAUDIO_DUMP. Only for the
    // "show me the samples" question: what the status block reports is our arithmetic over these
    // bytes, and this is the only way to check that the arithmetic and the bytes agree.
    FILE* dump = nullptr;
    unsigned long long dumpBytes = 0;
    unsigned fmtTag = 0, fmtBits = 0;
    bool fmtFloat = false;
    // The client is opened ON the render thread and this event is how `open` learns the result. It is
    // not tidiness: with the client created on the engine's loop thread and pumped from a second one,
    // every counter on this side read healthy - 48 000 frames/s written, zero underruns, post-gain peak
    // 1000/1000, the device's own clock advancing at exactly our rate - and the room was silent, while
    // a standalone program that creates, starts and pumps the client on one MTA thread was audible on
    // the same endpoint at the same session level. WASAPI clients are apartment-sensitive; the shape
    // that works is the one every documented sample uses.
    HANDLE ready = nullptr;
    std::atomic<long> openState{0};        // 0 = not yet, 1 = rendering, -1 = failed
    char openMsg[256] = {};
};

State g;

unsigned Backlog() {
    const unsigned long long n = g.wpos.load(std::memory_order_acquire)
                               - g.rpos.load(std::memory_order_acquire);
    return n > kRingFrames ? (unsigned)kRingFrames : (unsigned)n;
}

inline float* Slot(unsigned long long frame) {
    return g.ring.data() + (size_t)((frame & kRingMask) * g.ch);
}

// Pull `frames` out of the ring into the device packet. Short reads are filled with silence - the
// device cannot be given a partial packet - and every such fill is one underrun, which is the number
// that says whether the producer is keeping up.
void Drain(BYTE* dst, unsigned frames) {
    // The volume is approached, never jumped to. A wallpaper's sound starts and stops on decisions
    // the user cannot time (a game goes fullscreen, a monitor sleeps, the next card in the rotation
    // has no track), and a gain step at an arbitrary phase is a click. 10 ms of ramp is inaudible as
    // a fade and long enough to always land on a near-zero crossing of a 60 Hz-ish signal.
    const float target = g.paused.load(std::memory_order_relaxed)
                       ? 0.f : g.volume.load(std::memory_order_relaxed);
    const float step = 0.01f * static_cast<float>(g.rate) / 10.0f;   // full range in 10 ms
    float vol = g.volumeNow.load(std::memory_order_relaxed);
    if (vol < target) { vol += step; if (vol > target) vol = target; }
    else if (vol > target) { vol -= step; if (vol < target) vol = target; }
    g.volumeNow.store(vol, std::memory_order_relaxed);
    const unsigned long long r0 = g.rpos.load(std::memory_order_relaxed);
    const unsigned have = Backlog() < frames ? Backlog() : frames;
    float pk = 0.f;      // loudest sample actually handed to the device, post-gain

    if (g.pcm16) {
        INT16* out = reinterpret_cast<INT16*>(dst);
        for (unsigned f = 0; f < have; ++f) {
            const float* s = Slot(r0 + f);
            for (unsigned c = 0; c < g.ch; ++c) {
                float v = s[c] * vol;
                const float a = v < 0.f ? -v : v;
                if (a > pk) pk = a;
                if (v >  1.f) v =  1.f;
                if (v < -1.f) v = -1.f;
                out[f * g.ch + c] = (INT16)(v * 32767.f);
            }
        }
        for (unsigned i = have * g.ch; i < frames * g.ch; ++i) out[i] = 0;
    } else {
        float* out = reinterpret_cast<float*>(dst);
        for (unsigned f = 0; f < have; ++f) {
            const float* s = Slot(r0 + f);
            for (unsigned c = 0; c < g.ch; ++c) {
                const float v = s[c] * vol;
                const float a = v < 0.f ? -v : v;
                if (a > pk) pk = a;
                out[f * g.ch + c] = v;
            }
        }
        for (unsigned i = have * g.ch; i < frames * g.ch; ++i) out[i] = 0.f;
    }
    {
        const unsigned permille = (unsigned)(pk * 1000.f + 0.5f);
        if (permille > g.peak.load(std::memory_order_relaxed))
            g.peak.store(permille, std::memory_order_relaxed);
    }
    if (g.dump && g.bytesPerFrame) {
        fwrite(dst, 1, (size_t)frames * g.bytesPerFrame, g.dump);
        g.dumpBytes += (unsigned long long)frames * g.bytesPerFrame;
        if (g.dumpBytes > 8u << 20) { fclose(g.dump); g.dump = nullptr; }
    }
    if (have < frames && !g.paused.load(std::memory_order_relaxed)) g.underruns.fetch_add(1);
    // The ring's real content is consumed even when the rest of the packet was padded with silence,
    // so the read pointer is clamped to the write pointer. Advancing it by `frames` unconditionally
    // let it pass the write pointer on the very first short packet (one underrun at startup is enough),
    // after which `used = wpos - rpos` underflows, every later write sees zero room, and the producer
    // starves forever while every counter still reads healthy.
    const unsigned long long consumed = r0 + frames;
    const unsigned long long wNow = g.wpos.load(std::memory_order_relaxed);
    g.rpos.store(consumed < wNow ? consumed : wNow, std::memory_order_release);
}

static HRESULT OpenClient();   // defined below, next to the helpers it uses; runs on the render thread

DWORD WINAPI RenderThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT hr = OpenClient();
    g.openState.store(SUCCEEDED(hr) ? 1 : -1, std::memory_order_release);
    if (g.ready) SetEvent(g.ready);
    if (FAILED(hr)) { CoUninitialize(); return 0; }
    // Polled rather than event-driven. `GetEventHandle` lives on IAudioClient2 and the event-driven
    // path buys latency we do not need - a wallpaper is not a DAW - while a 10 ms poll against a
    // device-period buffer cannot starve it. One interface, one fewer thing that can be missing.
    while (!g.quit.load(std::memory_order_relaxed)) {
        Sleep(10);
        // The reported peak is "the loudest thing in roughly the last third of a second", not the
        // loudest thing since open - otherwise it would say 1000 forever after one loud frame.
        g.peak.store(g.peak.load(std::memory_order_relaxed) * 95u / 100u, std::memory_order_relaxed);
        UINT32 padding = 0;
        if (FAILED(g.client->GetCurrentPadding(&padding))) break;
        const unsigned space = (unsigned)(g.bufFrames - padding);
        if (space == 0) continue;
        BYTE* dst = nullptr;
        if (FAILED(g.render->GetBuffer(space, &dst))) continue;
        if (g.paused.load(std::memory_order_relaxed)
            && g.volumeNow.load(std::memory_order_relaxed) <= 0.f) {
            std::memset(dst, 0, (size_t)space * g.bytesPerFrame);
            g.rpos.store(g.wpos.load(std::memory_order_relaxed), std::memory_order_release);
        } else {
            // Drained even while pausing: the fade has to be rendered, not skipped.
            Drain(dst, space);
        }
        g.render->ReleaseBuffer(space, 0);
    }
    CoUninitialize();
    return 0;
}

bool MixIsFloat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    // cbSize counts the bytes AFTER the WAVEFORMATEX header, so an EXTENSIBLE format reports 22 here
    // while sizeof(WAVEFORMATEXTENSIBLE) is 40. Comparing the two made every real mix format - which
    // is EXTENSIBLE, 32-bit float on this machine - fall through to "not float", so we wrote int16
    // samples into a float buffer: the first eight frames of the dump decode as float32 are 0.0, the
    // counters all read healthy, and the room is silent.
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (unsigned)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
        return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat,
                           KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    // A header too short to carry a sub-format: believe the bit depth, which is the only thing left.
    return wf->wBitsPerSample == 32;
}

std::string Wide8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Which endpoint the sound is going to, and at what level. "没有任何声音" on a machine like this one
// has several quite different answers - physical speakers, the monitor over HDMI, a virtual display
// driver's nothing - and only the endpoint's own name tells them apart, so it is read back at open
// and carried into the status block instead of left for someone to guess from the mixer UI.
// Every step here is optional: none of it can stop the audio from working, and a failure just says
// "name?" / "level?" rather than being a reason to give up.
void DescribeEndpoint(IMMDevice* dev, char* out, unsigned bytes) {
    if (!dev || !bytes) return;
    out[0] = '\0';
    char name[96] = "name?";
    char lvl[64] = "level?";

    IPropertyStore* store = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kPKeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            const std::string n = Wide8(pv.pwszVal);
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s", n.c_str());
        }
        PropVariantClear(&pv);
        store->Release();
    }

    IAudioEndpointVolume* epv = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                (void**)&epv)) && epv) {
        float scalar = -1.f;
        BOOL mute = FALSE;
        epv->GetMasterVolumeLevelScalar(&scalar);
        epv->GetMute(&mute);
        if (scalar >= 0.f) _snprintf_s(lvl, sizeof(lvl), _TRUNCATE, "endpoint %d%%%s",
                                       (int)(scalar * 100.f + 0.5f), mute ? " MUTED" : "");
        epv->Release();
    }
    _snprintf_s(out, bytes, _TRUNCATE, "on \"%s\", %s", name, lvl);
}

// Our own process's mixer slot on the default endpoint. Windows lets the per-app volume sit at 0 while
// the master is at 80 %, so this is the one reading that can explain "every counter green, room silent".
// It cannot come from IAudioClient::GetService on this machine - that answers E_NOINTERFACE (0x80004002),
// which is why the status block used to say "app session?" - but the *device* will activate
// IAudioSessionManager2 directly, which is how the standalone probe read it.
void DescribeSession(char* out, unsigned bytes) {
    if (!out || !bytes) return;
    out[0] = '\0';
    IAudioSessionManager2* sm = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        _snprintf_s(out, bytes, _TRUNCATE, "app session: no enumerator");
        return;
    }
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) {
        en->Release();
        _snprintf_s(out, bytes, _TRUNCATE, "app session: no endpoint");
        return;
    }
    en->Release();
    const HRESULT ha = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&sm);
    dev->Release();
    if (FAILED(ha) || !sm) {
        _snprintf_s(out, bytes, _TRUNCATE, "app session: activate hr=0x%08lX", (unsigned long)ha);
        return;
    }
    float vol = -1.f;
    BOOL mute = FALSE;
    IAudioSessionEnumerator* se = nullptr;
    if (SUCCEEDED(sm->GetSessionEnumerator(&se)) && se) {
        int n = 0;
        se->GetCount(&n);
        const DWORD mine = GetCurrentProcessId();
        for (int i = 0; i < n; ++i) {
            IAudioSessionControl* sc = nullptr;
            if (FAILED(se->GetSession(i, &sc)) || !sc) continue;
            bool match = false;
            IAudioSessionControl2* sc2 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                DWORD pid = 0;
                match = SUCCEEDED(sc2->GetProcessId(&pid)) && pid == mine;
                sc2->Release();
            }
            if (match) {
                ISimpleAudioVolume* sav = nullptr;
                if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sav)) && sav) {
                    sav->GetMasterVolume(&vol);
                    sav->GetMute(&mute);
                    sav->Release();
                }
                sc->Release();
                break;
            }
            sc->Release();
        }
        se->Release();
    }
    sm->Release();
    if (vol >= 0.f) _snprintf_s(out, bytes, _TRUNCATE, "app %d%%%s",
                                (int)(vol * 100.f + 0.5f), mute ? " MUTED" : "");
    else _snprintf_s(out, bytes, _TRUNCATE, "no mixer slot for this process yet");
}

// Everything COM, on the thread that will also pump the buffers: enumerate the endpoint, activate the
// client, agree the format, start, and then loop. `open` waits on g.ready for this to finish and
// reports what it said. Failures land in g.openMsg, which the caller copies into its own error buffer.
static HRESULT OpenClient() {
    auto note = [&](const char* m, HRESULT h) {
        _snprintf_s(g.openMsg, sizeof(g.openMsg), _TRUNCATE, "%s (hr=0x%08lX)", m, (unsigned long)h);
        if (g.client) { g.client->Release(); g.client = nullptr; }
        return h;
    };

    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) return note("CoCreateInstance(MMDeviceEnumerator)", hr);
    IMMDevice* dev = nullptr;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr)) return note("no default render endpoint", hr);

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g.client);
    char route[256] = "";
    DescribeEndpoint(dev, route, sizeof(route));   // before the device goes away
    dev->Release();
    if (FAILED(hr)) return note("IAudioClient activate", hr);

    WAVEFORMATEX* wf = nullptr;
    hr = g.client->GetMixFormat(&wf);
    if (FAILED(hr) || !wf) return note("GetMixFormat", hr);
    g.rate = wf->nSamplesPerSec;
    g.ch   = wf->nChannels;
    g.pcm16 = !MixIsFloat(wf);
    g.fmtTag = wf->wFormatTag;
    g.fmtBits = wf->wBitsPerSample;
    g.fmtFloat = !g.pcm16;
    g.bytesPerFrame = g.ch * (g.pcm16 ? 2u : 4u);
    g.ring.assign((size_t)(kRingFrames * g.ch), 0.f);
    {
        char dumpPath[260] = "";
        if (GetEnvironmentVariableA("SWAUDIO_DUMP", dumpPath, sizeof(dumpPath)) > 0
            && strlen(dumpPath) < sizeof(dumpPath)) {
            // fopen_s rather than fopen: /W4 flags the latter as C4996. It leaves the pointer null on
            // failure, which is what the setvbuf gate below already assumes.
            fopen_s(&g.dump, dumpPath, "wb");
            // Unbuffered on purpose: the capture has to survive the process being killed, and the
            // whole point is to look at bytes that were really handed over.
            if (g.dump) setvbuf(g.dump, nullptr, _IONBF, 0);
        }
    }
    const HRESULT hinit = g.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferHns, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hinit)) return note("IAudioClient::Initialize", hinit);

    hr = g.client->GetService(__uuidof(IAudioRenderClient), (void**)&g.render);
    if (FAILED(hr)) return note("IAudioRenderClient", hr);
    // Optional: no clock just means the played-frame number is unavailable, never means no audio.
    g.client->GetService(__uuidof(IAudioClock), (void**)&g.clock);
    UINT32 bf = 0;
    hr = g.client->GetBufferSize(&bf);
    if (FAILED(hr) || bf == 0) return note("GetBufferSize", hr);
    g.bufFrames = bf;
    g.latencyMs.store((unsigned)((unsigned long long)bf * 10000000ull / g.rate / 10000ull));

    hr = g.client->Start();
    if (FAILED(hr)) return note("Start", hr);

    // On success this buffer is not an error at all, it is the route: what AudioOut shows as the
    // backend's own line. It answers "which speakers, at what level, and is Windows muting just us".
    char app[96] = "";
    DescribeSession(app, sizeof(app));
    _snprintf_s(g.openMsg, sizeof(g.openMsg), _TRUNCATE, "%s, %s", route, app);
    return S_OK;
}

int Open(unsigned* rateOut, unsigned* chOut, char* err, unsigned errBytes) {
    if (g.client) return 0;                       // the engine opens it once; a second call is a no-op
    g.quit = 0;
    g.openState = 0;
    g.openMsg[0] = '\0';
    if (!g.ready) g.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g.ready) {
        _snprintf_s(g.openMsg, sizeof(g.openMsg), _TRUNCATE, "CreateEvent (err=%lu)", GetLastError());
    }
    g.thread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
    if (!g.thread) {
        const HRESULT h = HRESULT_FROM_WIN32(GetLastError());
        if (err && errBytes) _snprintf_s(err, errBytes, _TRUNCATE, "CreateThread (hr=0x%08lX)", (unsigned long)h);
        return -1;
    }
    // The client only exists once the render thread has built it; two seconds covers a device that is
    // slow to answer, and a timeout is reported as "no audio" rather than hanging the wallpaper.
    if (g.ready && WaitForSingleObject(g.ready, 2000) == WAIT_TIMEOUT) {
        if (err && errBytes) _snprintf_s(err, errBytes, _TRUNCATE, "backend open timed out");
        return -1;
    }
    if (g.openState.load() != 1) {
        if (err && errBytes) _snprintf_s(err, errBytes, _TRUNCATE, "%s", g.openMsg);
        return -1;
    }
    // On success g.openMsg carries the route, not an error.
    if (err && errBytes) _snprintf_s(err, errBytes, _TRUNCATE, "%s", g.openMsg);
    if (rateOut) *rateOut = g.rate;
    if (chOut) *chOut = g.ch;
    return 0;
}

int Write(const float* pcm, unsigned frames) {
    if (!g.client || !pcm || frames == 0) return 0;
    // The ring is only sized once the endpoint has told us its channel count, which happens on the
    // render thread. Before that, "no room" is the honest answer.
    if (g.openState.load(std::memory_order_acquire) != 1) return 0;
    // While paused the render thread discards the backlog so that resuming plays what is current. If
    // this also accepted samples, the ring could never fill, so a producer started during a pause
    // would run flat out with nothing thrown away - measured: a 5 s tone that wrote 17,921,024 frames
    // (373 s of audio) because the discard kept the room permanently full. Silence is back-pressure.
    if (g.paused.load(std::memory_order_relaxed)) return 0;
    const unsigned long long w0 = g.wpos.load(std::memory_order_relaxed);
    // Clamped both ways: the read pointer is kept at or behind the write pointer by Drain, and this
    // guard means a future slip shows up as "no room for now" instead of an unsigned wrap that reads
    // as a full ring forever.
    const unsigned long long r0 = g.rpos.load(std::memory_order_acquire);
    const unsigned long long used = w0 > r0 ? w0 - r0 : 0;
    const unsigned long long room = used < kRingFrames ? kRingFrames - used : 0;
    const unsigned take = (unsigned)(frames < room ? frames : room);
    for (unsigned f = 0; f < take; ++f) {
        float* d = Slot(w0 + f);
        const float* s = pcm + (size_t)f * g.ch;
        for (unsigned c = 0; c < g.ch; ++c) d[c] = s[c];
    }
    g.wpos.store(w0 + take, std::memory_order_release);
    return (int)take;
}

// The route as of this call, from a fresh look at the default endpoint - not what we saw at open.
// Cheap enough to run only when someone asks the engine what is playing: one enumerator, one device,
// one property store, one volume object, all released here.
void Route(char* out, unsigned bytes) {
    if (!out || !bytes) return;
    out[0] = '\0';
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        _snprintf_s(out, bytes, _TRUNCATE, "route: no enumerator");
        return;
    }
    IMMDevice* dev = nullptr;
    const HRESULT hget = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hget) || !dev) {
        _snprintf_s(out, bytes, _TRUNCATE, "route: no default render endpoint hr=0x%08lX",
                    (unsigned long)hget);
        return;
    }
    char r[256] = "";
    DescribeEndpoint(dev, r, sizeof(r));
    char app[96] = "";
    DescribeSession(app, sizeof(app));
    // Frames the device reports as played, at the clock's own rate converted to our frame count.
    unsigned long long playedFrames = 0;
    unsigned long long clockFreq = 0;
    if (g.clock) {
        UINT64 f = 0;
        if (SUCCEEDED(g.clock->GetFrequency(&f))) clockFreq = (unsigned long long)f;
        UINT64 pos = 0, drop = 0;
        if (SUCCEEDED(g.clock->GetPosition(&pos, &drop)) && g.rate && clockFreq) {
            playedFrames = (unsigned long long)((double)pos * (double)g.rate / (double)clockFreq);
        }
    }
    // The backend's own live state, next to the endpoint: what the engine asked for, what is actually
    // being applied, and whether the hold flag is up. Peak 0 with these numbers tells which of the
    // ways to write silence this is. `played` is in frames only because `freq` says what the clock's
    // own units are - assuming 100 ns instead of reading it is how a device position gets mis-scaled.
    _snprintf_s(out, bytes, _TRUNCATE, "%s, %s, asked=%d permille, applied=%d, paused=%d, played=%llu, freq=%llu, fmt=tag %u bits %u %s %uch %uHz",
                r, app, (int)(g.volume.load(std::memory_order_relaxed) * 1000.f),
                (int)(g.volumeNow.load(std::memory_order_relaxed) * 1000.f),
                (int)g.paused.load(std::memory_order_relaxed), playedFrames, clockFreq,
                g.fmtTag, g.fmtBits, g.fmtFloat ? "float" : "pcm16", g.ch, g.rate);
    dev->Release();
}

void SetVolume(float v) {    g.volume.store(v < 0.f ? 0.f : (v > 1.f ? 1.f : v), std::memory_order_relaxed);
}
void SetPaused(int p) { g.paused.store(p ? 1 : 0, std::memory_order_relaxed); }
unsigned Underruns() { return g.underruns.load(std::memory_order_relaxed); }
unsigned LatencyMs() { return g.latencyMs.load(std::memory_order_relaxed); }
unsigned Peak() { return g.peak.load(std::memory_order_relaxed); }

void Close() {
    // The thread may have exited on its own (an endpoint that refused to open), and it exists even when
    // the client does not - so it is joined before anything is released, not after the `no client`
    // early-out that used to leave it running against a freed ring.
    g.quit.store(1);
    if (g.thread) { WaitForSingleObject(g.thread, 1000); CloseHandle(g.thread); g.thread = nullptr; }
    g.openState = 0;
    if (g.dump) { fclose(g.dump); g.dump = nullptr; }
    if (g.ready) { CloseHandle(g.ready); g.ready = nullptr; }
    if (!g.client) return;
    g.client->Stop();
    if (g.render) { g.render->Release(); g.render = nullptr; }
    if (g.clock) { g.clock->Release(); g.clock = nullptr; }
    g.client->Release();
    g.client = nullptr;
}

const SwAudioPlugin kPlugin = {
    SWAUDIO_ABI, "wasapi-shared", Open, Write, SetVolume, SetPaused, Underruns, LatencyMs, Route,
    Peak, Close
};

} // namespace

SWAUDIO_EXPORT const SwAudioPlugin* swaudio_get() { return &kPlugin; }
