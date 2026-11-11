#include "frame_pacer.hpp"
#include <iostream>

using namespace std::chrono_literals;
using Pacer = luna::video::FramePacer;
namespace
{
unsigned checks = 0;
void check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    const Pacer::Time origin{};
    Pacer pacer;
    check(pacer.delay(origin, origin + 5ms, 20ms) == 15ms,
        "First frame subtracts all processing, including applet work");
    check(pacer.delay(origin + 23ms, origin + 28ms, 20ms) == 12ms,
        "Late wake-up shortens the next sleep instead of moving the timeline");
    check(pacer.delay(origin + 40ms, origin + 60ms, 20ms) == 0ns,
        "Work that already reached the deadline adds no second wait");
    check(pacer.delay(origin + 60ms, origin + 65ms, 20ms) == 15ms,
        "Normal pacing resumes after a deadline is reached");

    pacer.reset();
    check(pacer.delay(origin, origin + 205ms, 20ms) == 0ns,
        "A long stall skips expired ticks without replaying them");
    check(pacer.delay(origin + 205ms, origin + 210ms, 20ms) == 10ms,
        "The iteration after a long stall sleeps toward a future deadline");
    check(pacer.delay(origin + 220ms, origin + 225ms, 40ms) == 35ms,
        "Changing video rate starts a new timeline");
    check(pacer.delay(origin + 10ms, origin + 15ms, 40ms) == 35ms,
        "A clock rollback resets stale deadlines");
    pacer.reset();
    check(pacer.delay(origin + 1000ms, origin + 1004ms, 20ms) == 16ms,
        "Returning from a hidden video view starts with a fresh timeline");

    for (const int fps : {30, 60})
    {
        pacer.reset();
        const auto period = std::chrono::duration_cast<Pacer::Duration>(1s) / fps;
        auto now = origin;
        // Model applet IPC, frame work, scheduler jitter and occasional
        // presentation blocking. No real sleeps or Switch hardware are used.
        for (int frame = 0; frame < 180; ++frame)
        {
            const auto started = now;
            now += 2ms; // This was outside the old timer.
            now += frame % 7 == 0 ? 9ms : 6ms;
            const auto wait = pacer.delay(started, now, period);
            check(wait >= 0ns && wait <= period, "Pacing waits stay bounded to one frame");
            now += wait;
            if (wait > 0ns) now += 1ms + 200us * (frame % 4);
        }
        const auto elapsed = now - origin;
        check(elapsed >= 180 * period && elapsed - 180 * period <= 2ms,
            "Scheduler jitter does not accumulate across a 30/60 FPS run");
    }

    pacer.reset();
    auto now = origin;
    for (int frame = 0; frame < 20; ++frame)
    {
        const auto started = now; now += 40ms;
        check(pacer.delay(started, now, 16ms) == 0ns,
            "Sustained slow rendering is not made slower by extra frame sleeps");
    }
    for (const auto invalid : {0ns, -1ns})
    {
        bool rejected = false;
        try { pacer.delay(origin, origin, invalid); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Nonpositive frame periods are rejected");
    }
    bool rejected = false;
    try { pacer.delay(origin + 1ms, origin, 20ms); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "A negative work duration is rejected");
    std::cout << "PASS: " << checks << " frame deadline, jitter, stall, reset and rate-change checks (simulated clock).\n";
}
