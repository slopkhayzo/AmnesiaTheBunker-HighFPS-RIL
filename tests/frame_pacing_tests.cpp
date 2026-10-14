#include "frame_pacing.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

bool Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

constexpr std::int64_t kFrequency = 10'000'000;
constexpr std::int64_t kTick = kFrequency / 60;     // 60 Hz simulation
constexpr std::int64_t kMinimum = kFrequency / 350; // 350 FPS cap
constexpr std::int64_t kSwap = 2'000;               // 0.2 ms
constexpr std::int64_t kRender = 20'000;            // 2.0 ms
constexpr std::int64_t kJitter = 6;                 // rounding of the grid

struct Run {
    unsigned int framesPerTick = 0;
    std::int64_t shortestTick = 0;
    std::int64_t longestTick = 0;
    std::int64_t shortestPlain = 0;
    std::int64_t longestPlain = 0;
    bool tickAlwaysAtFrameStart = true;
};

void Widen(std::int64_t& shortest,
           std::int64_t& longest,
           const std::int64_t value) {
    shortest = shortest == 0 ? value : std::min(shortest, value);
    longest = std::max(longest, value);
}

// Plays the engine's loop against the pacer: poll the tick at the start of
// each frame, run it if due, wait for the render start, render, wait for the
// deadline, present. Time carries over between calls.
std::int64_t g_now = 1'000'000;
std::int64_t g_due = g_now + kTick;
std::int64_t g_frameStart = 0;

Run Simulate(atb::pacing::Pacer& pacer,
             const std::int64_t updateCost,
             const int ticks) {
    Run run{};
    std::int64_t lastPresent = g_frameStart;
    int ticksRun = 0;
    while (ticksRun < ticks) {
        unsigned int frameTicks = 0;
        const bool dueAtStart = g_now >= g_due;
        if (dueAtStart) {
            g_now += updateCost;
            g_due += kTick;
            ++frameTicks;
            ++ticksRun;
        }
        const atb::pacing::Plan plan = pacer.BeginFrame(g_now, g_due, kTick);
        if (!plan.valid) {
            return run;
        }
        std::int64_t waited = 0;
        if (g_now < plan.renderStart) {
            waited = plan.renderStart - g_now;
            g_now = plan.renderStart;
        }
        const std::int64_t renderStart = g_now;
        g_now += kRender;
        if (g_frameStart != 0) {
            pacer.EndFrame(g_now - g_frameStart - waited, g_now - renderStart,
                           frameTicks);
        }
        g_now = std::max(g_now, plan.deadline);
        // Only judge the steady state, after the pacer has had time to adapt.
        if (ticksRun > ticks - 60 && lastPresent != 0) {
            if (frameTicks != 0) {
                Widen(run.shortestTick, run.longestTick, g_now - lastPresent);
            } else {
                Widen(run.shortestPlain, run.longestPlain, g_now - lastPresent);
            }
            // A frame that starts with the tick overdue by a whole frame
            // means the tick fell due in the middle of the one before.
            if (!dueAtStart && g_now - kSwap > g_due + kSwap) {
                run.tickAlwaysAtFrameStart = false;
            }
        }
        lastPresent = g_now;
        g_frameStart = g_now;
        g_now += kSwap;
    }
    run.framesPerTick = pacer.FramesPerTick();
    return run;
}

bool Even(const Run& run) {
    return run.longestTick - run.shortestTick <= kJitter &&
           run.longestPlain - run.shortestPlain <= kJitter &&
           std::abs(run.longestTick - run.longestPlain) <= kJitter;
}

} // namespace

int main() {
    bool ok = true;
    atb::pacing::Pacer pacer;
    ok &= Require(!pacer.Configure(0, kMinimum, 100) &&
                      !pacer.Configure(kFrequency, 0, 100) &&
                      !pacer.Configure(kFrequency, kMinimum, 0) &&
                      !pacer.Configure(kFrequency, kMinimum, 101),
                  "invalid configuration should be rejected");
    ok &= Require(pacer.Configure(kFrequency, kMinimum, 100),
                  "valid configuration should be accepted");
    ok &= Require(!pacer.BeginFrame(1'000'000, 1'000'000 + 10 * kTick, kTick).valid,
                  "a due time far from now should not be paced");
    ok &= Require(!pacer.BeginFrame(1'000'000, 1'000'000, 0).valid,
                  "a missing tick period should not be paced");

    // Fully even. A cheap update: the cap decides, 16.67 ms / 2.857 ms
    // allows five frames per tick.
    ok &= Require(pacer.Configure(kFrequency, kMinimum, 100), "reconfigure");
    Run run = Simulate(pacer, 2'000, 600);
    ok &= Require(run.framesPerTick == 5 && Even(run),
                  "a cheap update should give five equal frames per tick");

    // A 2.6 ms update next to a 2.2 ms frame fits three times, not four.
    run = Simulate(pacer, 26'000, 600);
    ok &= Require(run.framesPerTick == 3 && Even(run),
                  "a 2.6 ms update should give three equal frames per tick");
    ok &= Require(std::abs(run.longestTick - kTick / 3) <= kJitter,
                  "three equal frames should each last a third of a tick");
    ok &= Require(run.tickAlwaysAtFrameStart,
                  "ticks should fall due at the start of a frame");

    // A 3.6 ms update makes the tick frame 5.8 ms: more than a third of a
    // tick, so full evenness has to fall back to two frames per tick.
    run = Simulate(pacer, 36'000, 600);
    ok &= Require(run.framesPerTick == 2 && Even(run),
                  "full evenness should drop to two frames per tick");
    run = Simulate(pacer, 26'000, 900);
    ok &= Require(run.framesPerTick == 3,
                  "the count should rise again when there is room");

    // Evenness 75 lets the tick frame be up to 25% longer: the same 5.8 ms
    // tick frame now keeps three frames per tick, the other two sharing the
    // rest of the tick equally.
    ok &= Require(pacer.Configure(kFrequency, kMinimum, 75), "reconfigure");
    run = Simulate(pacer, 36'000, 900);
    ok &= Require(run.framesPerTick == 3,
                  "evenness 75 should keep three frames per tick");
    ok &= Require(run.longestTick - run.shortestTick <= kJitter &&
                      run.longestPlain - run.shortestPlain <= kJitter,
                  "tick frames and plain frames should each be regular");
    ok &= Require(run.longestTick > run.longestPlain &&
                      run.longestTick * 100 <= run.longestPlain * 126,
                  "the tick frame should be at most 25% longer");
    ok &= Require(std::abs(run.longestTick + 2 * run.longestPlain - kTick) <=
                      3 * kJitter,
                  "the three frames should add up to one tick");
    ok &= Require(run.tickAlwaysAtFrameStart,
                  "ticks should still fall due at the start of a frame");

    // A costlier tick stretches the tick frame instead of giving up a frame:
    // 6.8 ms is 39% longer than the two frames left, inside twice the 25%.
    run = Simulate(pacer, 46'000, 600);
    ok &= Require(run.framesPerTick == 3,
                  "a tick frame inside twice the tolerance should keep its "
                  "frames");
    ok &= Require(run.longestTick > run.longestPlain * 130 / 100 &&
                      run.longestTick - run.shortestTick <= kJitter &&
                      run.longestPlain - run.shortestPlain <= kJitter,
                  "the stretched tick frame should stay regular");
    // Beyond twice the tolerance a frame is given up, and it comes back
    // once the tick is cheap enough for the tolerance itself.
    run = Simulate(pacer, 56'000, 600);
    ok &= Require(run.framesPerTick == 2 && Even(run),
                  "a tick frame beyond twice the tolerance should drop one");
    run = Simulate(pacer, 46'000, 600);
    ok &= Require(run.framesPerTick == 2,
                  "between the two limits the count should not change");
    run = Simulate(pacer, 36'000, 600);
    ok &= Require(run.framesPerTick == 3,
                  "inside the tolerance the frame should come back");

    // Evenness 50 trades more: four frames per tick with the 2.6 ms update.
    ok &= Require(pacer.Configure(kFrequency, kMinimum, 50), "reconfigure");
    run = Simulate(pacer, 26'000, 900);
    ok &= Require(run.framesPerTick == 4,
                  "evenness 50 should present four frames per tick");
    ok &= Require(run.longestTick * 100 <= run.longestPlain * 151 &&
                      run.shortestPlain >= kMinimum,
                  "the tick frame should be at most 50% longer and the cap "
                  "respected");

    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "frame pacing tests passed\n";
    return EXIT_SUCCESS;
}
