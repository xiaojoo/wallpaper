#pragma once
// Engine/Performance/FPSController.hpp - one pacer for the thread, per-slot frame budgets.
#include "Engine/Core/Platform.hpp"

namespace sw {

// Each monitor gets its own interval and its own measured frame rate; the thread waits only until
// the earliest one is due. Pacing is timer-based so an idle desktop costs no CPU between frames.
class FPSController {
public:
    struct Slot {
        int targetFps = 0;       // what the wallpaper asked for, before power rules
        int effectiveFps = 0;    // after power caps, and snapped to whole refresh periods
        LONGLONG intervalTicks = 0;
        LONGLONG nextDue = 0;
        unsigned long long frames = 0;
        double measuredFps = 0, avgMs = 0, worstMs = 0;
        // Frames that landed more than half an interval late, since the budget last changed: a
        // maximum only remembers the worst gap ever, and SlotIdle (which zeroes lastFrame for any
        // slot not due this iteration) would blank a lastFrame-based counter on the second monitor
        // and on the preview pass.
        unsigned lateFrames = 0;
        LONGLONG lastReal = 0;
        LONGLONG lastFrame = 0, windowStart = 0;
        unsigned long long windowFrames = 0;
        bool paused = false;
    };

    bool Init(std::string& error);
    void Shutdown();

    LONGLONG Now() const;
    void ConfigureSlot(Slot& s, int targetFps, UINT refreshHz);
    bool SlotDue(const Slot& s) const;
    void SlotRendered(Slot& s);
    // Called for slots that were not due: lets the measured rate decay to 0 while paused
    // and drops the idle gap from the frame-time statistics.
    void SlotIdle(Slot& s);
    LONGLONG EarliestDue(const std::vector<Slot*>& slots) const;
    // Waits until `due`. Returns true when a message woke us (the caller drains and re-plans) and
    // false when the frame is due. Wakes at least every MessageQuantumMs either way: our window is a
    // child of Progman, and a cross-thread SendMessage is only answered by GetMessage/PeekMessage,
    // which MsgWaitForMultipleObjects does not wake for.
    bool WaitUntil(LONGLONG due);
    // Idle wait that still keeps the message queue serviced (nothing may draw right now).
    void WaitSlice(double seconds);

    static constexpr DWORD MessageQuantumMs = 16;
    static std::string Describe(const Slot& s, std::string_view label);

    double qpcToMs(LONGLONG ticks) const { return double(ticks) * 1000.0 / double(qpcFreq_); }

private:
    HANDLE timer_ = nullptr;
    LONGLONG qpcFreq_ = 1;
};

} // namespace sw
