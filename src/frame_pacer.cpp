#include "frame_pacer.h"
#include "settings.h"
#include <game_window.h>
#include <atomic>
#include <chrono>
#include <thread>

using Clock = std::chrono::steady_clock;

static std::atomic<int> explicitCap{0};
static std::atomic<double> fps{0.0};
static Clock::time_point nextFrame = Clock::now();
static Clock::time_point windowStart = Clock::now();
static int framesInWindow = 0;

void FramePacer::setCap(int cap) {
    explicitCap = cap < 0 ? 0 : cap;
}

int FramePacer::getCap() {
    return explicitCap;
}

int FramePacer::activeCap(GameWindow *window) {
    int cap = explicitCap;
    if(cap > 0)
        return cap;
    if(window && !window->isFocused())
        return Settings::unfocused_fps_cap;
    return 0;
}

void FramePacer::afterSwap(GameWindow *window) {
    auto now = Clock::now();
    if(++framesInWindow >= 10 || now - windowStart >= std::chrono::seconds(1)) {
        auto secs = std::chrono::duration<double>(now - windowStart).count();
        if(secs > 0)
            fps = framesInWindow / secs;
        framesInWindow = 0;
        windowStart = now;
    }
    int cap = activeCap(window);
    if(cap <= 0) {
        nextFrame = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000 / cap);
    // Schedule from the previous deadline so the cap holds steady instead of drifting with sleep jitter.
    if(nextFrame + period < now)
        nextFrame = now;
    nextFrame += period;
    std::this_thread::sleep_until(nextFrame);
}

double FramePacer::measuredFps() {
    return fps;
}
