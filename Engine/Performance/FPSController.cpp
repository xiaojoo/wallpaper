#include "Engine/Performance/FPSController.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "fps";

bool FPSController::Init(std::string& error) {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    qpcFreq_ = li.QuadPart ? li.QuadPart : 1;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!timer_) {
            error = "CreateWaitableTimer: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        Warn(MOD, "high-resolution waitable timer unavailable - frame pacing will be coarse");
    }
    Info(MOD, "pacer ready, qpc frequency {} Hz", qpcFreq_);
    return true;
}

void FPSController::Shutdown() {
    if (timer_) {
        CancelWaitableTimer(timer_);
        CloseHandle(timer_);
        timer_ = nullptr;
    }
}

LONGLONG FPSController::Now() const {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

namespace {
// The fastest rate at or below `want` that is an exact whole number of refresh periods. 60 fps on a
// 144 Hz panel is 2.4 refreshes per frame, so the compositor shows the frames 2, 3, 2, 3 refreshes
// long - a regular 13.9/20.8 ms judder that reads as stutter however steadily the loop delivers.
// 144 Hz therefore gives 48 (3 periods), 24 (6), 12 (12). If the mode reports a refresh with no
// divisor near what was asked for (143 Hz: 11 and 13), keeping the requested rate beats dropping
// to a twentieth of it, so the snap only applies within a quarter of `want`.
int SnapToRefresh(int want, int refresh) {
    for (int d = want; d * 4 >= want * 3; --d)
        if (refresh % d == 0) return d;
    return want;
}
} // namespace

void FPSController::ConfigureSlot(Slot& s, int targetFps, UINT refreshHz) {
    int want = targetFps < 0 ? 0 : targetFps;
    int cap = refreshHz ? int(refreshHz) : 60;
    int eff = want == 0 ? 0 : SnapToRefresh(std::min(want, cap), cap);
    s.targetFps = want;
    if (eff == s.effectiveFps) return;
    s.effectiveFps = eff;
    s.intervalTicks = eff > 0 ? qpcFreq_ / eff : 0;
    s.paused = eff == 0;
    // A budget change is not a hitch: the first interval after one is measured against the new
    // rate, and the late counter restarts so an A/B over a stable window reads on its own.
    s.lastFrame = 0;
    s.lastReal = 0;
    s.lateFrames = 0;
    // A budget change must never push away a frame that is already due: the power cap flips
    // between states as the foreground window changes, and re-phasing here starved the loop.
    if (s.nextDue <= Now() || s.nextDue == 0) s.nextDue = Now();
}

bool FPSController::SlotDue(const Slot& s) const {
    return s.effectiveFps > 0 && Now() >= s.nextDue;
}

void FPSController::SlotRendered(Slot& s) {
    LONGLONG now = Now();
    if (s.lastFrame) {
        double ms = double(now - s.lastFrame) * 1000.0 / qpcFreq_;
        // Only count it as a frame time when it is plausibly one; a gap after a pause or a
        // cap change is idle time, and it made "worst frame" read 62840 ms.
        double budget = s.effectiveFps > 0 ? 3000.0 / s.effectiveFps : 1000.0;
        if (ms <= budget) {
            s.worstMs = s.worstMs < ms ? ms : s.worstMs;
            s.avgMs = s.avgMs ? s.avgMs * 0.9 + ms * 0.1 : ms;
        }
    }
    if (s.effectiveFps > 0 && s.lastReal) {
        // More than half an interval late means the frame was held a whole refresh too long.
        double ideal = 1000.0 / s.effectiveFps;
        double gap = double(now - s.lastReal) * 1000.0 / qpcFreq_;
        if (gap > ideal * 1.5 && gap < ideal * 30.0) ++s.lateFrames;
    }
    s.lastReal = now;
    s.lastFrame = now;
    ++s.frames;

    LONGLONG span = now - s.windowStart;
    if (span >= qpcFreq_) {
        s.measuredFps = double(s.frames - s.windowFrames) * double(qpcFreq_) / double(span ? span : 1);
        s.windowStart = now;
        s.windowFrames = s.frames;
    }

    s.nextDue += s.intervalTicks;
    if (s.nextDue < now) s.nextDue = now + s.intervalTicks; // drop overdue frames, never spiral
}

void FPSController::SlotIdle(Slot& s) {
    LONGLONG now = Now();
    if (now - s.windowStart >= qpcFreq_) {
        s.measuredFps = double(s.frames - s.windowFrames) * double(qpcFreq_) / double(now - s.windowStart);
        s.windowStart = now;
        s.windowFrames = s.frames;
    }
    if (s.effectiveFps <= 0) s.measuredFps = 0.0;
    s.lastFrame = 0; // the next gap is idle time, not a frame
}

LONGLONG FPSController::EarliestDue(const std::vector<Slot*>& slots) const {
    LONGLONG best = 0;
    for (auto* s : slots) {
        if (s->effectiveFps <= 0) continue;
        if (!best || s->nextDue < best) best = s->nextDue;
    }
    return best;
}

bool FPSController::WaitUntil(LONGLONG due) {
    // Why this never sleeps longer than one quantum: our window is a child of Progman, so Explorer's
    // desktop refresh and context menu send messages straight to it, and a sent message is only
    // dispatched by GetMessage/PeekMessage. MsgWaitForMultipleObjects ignores them (QS_SENDMESSAGE is
    // not supported), so one long sleep made every one of those operations wait for our next frame -
    // 90 ms at 10 FPS. Returning early hands control to the caller's DrainMessages, then we wait again.
    LONGLONG now = Now();
    if (!due || due <= now) return false;
    LONGLONG remaining = due - now;
    LONGLONG quantum = (qpcFreq_ / 1000) * MessageQuantumMs;
    LONGLONG slice = std::min(remaining, quantum);

    LARGE_INTEGER rel{};
    rel.QuadPart = -std::max<LONGLONG>(1, (slice * 10000000LL) / qpcFreq_);
    if (timer_) SetWaitableTimer(timer_, &rel, 0, nullptr, nullptr, FALSE);

    DWORD ms = DWORD(slice / (qpcFreq_ / 1000) + 1);
    HANDLE h = timer_;
    // WAIT_OBJECT_0 + 1 is the message branch: the caller drains the queue before drawing.
    DWORD r = h ? MsgWaitForMultipleObjects(1, &h, FALSE, ms, QS_ALLEVENTS) : (Sleep(ms), WAIT_TIMEOUT);
    if (r == WAIT_OBJECT_0 + 1) return true;
    return due > Now(); // woke on a quantum, not on the frame: re-plan rather than draw early
}

void FPSController::WaitSlice(double seconds) {
    WaitUntil(Now() + LONGLONG(qpcFreq_ * seconds));
}

std::string FPSController::Describe(const Slot& s, std::string_view label) {
    return std::format("{} target={} eff={} measured={:.1f} avg={:.2f}ms worst={:.2f}ms late={} frames={}", label,
                       s.targetFps, s.effectiveFps, s.measuredFps, s.avgMs, s.worstMs, s.lateFrames, s.frames);
}

} // namespace sw
