#include "presentation_clock.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int g_failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void TestCadence(const unsigned int gameplayRate,
                 const unsigned int presentationRate) {
    using atb::clock::PresentationClock;
    using atb::clock::SampleKind;

    const double nativeStep = 1.0 / static_cast<double>(gameplayRate);
    const double presentationStep = 1.0 / static_cast<double>(presentationRate);
    PresentationClock clock;
    Expect(clock.Configure(nativeStep), "valid native rate should configure");

    std::uint64_t serial = 0;
    double nextTick = 0.0;
    double latestTick = 0.0;
    double previousTarget = -1.0;
    unsigned int interpolated = 0;
    for (double now = 0.0; now <= 3.0; now += presentationStep) {
        while (nextTick <= now + 1.0e-9) {
            latestTick = nextTick;
            Expect(clock.Publish(serial++, nextTick).accepted,
                   "steady native tick should publish");
            nextTick += nativeStep;
        }
        const double alpha = (now - latestTick) / nativeStep;
        const auto sample = clock.Evaluate(now, alpha);
        Expect(sample.kind != SampleKind::reset,
               "steady cadence must not reset");
        if (sample.kind == SampleKind::interpolated) {
            Expect(sample.alpha >= 0.0 && sample.alpha <= 1.0,
                   "interpolation alpha must remain bounded");
            Expect(sample.targetTime + 1.0e-8 >= previousTarget,
                   "presented simulation time must not regress");
            previousTarget = sample.targetTime;
            ++interpolated;
        }
    }
    Expect(interpolated > presentationRate * 2,
           "steady cadence should produce interpolation samples for most frames at " +
               std::to_string(gameplayRate) + "/" +
               std::to_string(presentationRate) + " Hz");
}

void TestConfigurationAndResetPaths() {
    using atb::clock::PresentationClock;
    using atb::clock::ResetReason;
    using atb::clock::SampleKind;

    PresentationClock unconfigured;
    Expect(!unconfigured.Publish(0, 0.0).accepted,
           "an unconfigured clock must fail closed");
    Expect(!unconfigured.Configure(0.0), "zero native step must be rejected");
    Expect(!unconfigured.Configure(1.0 / 60.0, 0.001),
           "stall threshold below native step must be rejected");

    PresentationClock clock;
    Expect(clock.Configure(1.0 / 120.0), "120 Hz native step should configure");
    const double step = clock.NativeStepSeconds();
    (void)clock.Publish(0, 0.0);
    (void)clock.Publish(1, step);
    const auto sample = clock.Evaluate(1.5 * step, 0.5);
    Expect(sample.kind == SampleKind::interpolated &&
               std::abs(sample.targetTime - 0.5 * step) < 1.0e-10,
           "configured native step must drive target time");

    const auto gap = clock.Publish(3, 2.0 * step);
    Expect(gap.accepted && gap.resetReason == ResetReason::missingSnapshots &&
               clock.SnapshotCount() == 1,
           "a missing fixed state must reset and retain only the new endpoint");

    PresentationClock stalled;
    (void)stalled.Configure(1.0 / 60.0);
    (void)stalled.Publish(0, 0.0);
    (void)stalled.Publish(1, 1.0 / 60.0);
    (void)stalled.Evaluate(2.0 / 60.0, 1.0);
    const auto afterStall = stalled.Evaluate(0.300, 1.0);
    Expect(afterStall.kind == SampleKind::reset &&
               afterStall.resetReason == ResetReason::longStall,
           "long presentation stall must clear stale snapshots");

    PresentationClock invalid;
    (void)invalid.Configure(1.0 / 30.0);
    const auto bad = invalid.Publish(
        0, std::numeric_limits<double>::quiet_NaN());
    Expect(!bad.accepted &&
               bad.resetReason == ResetReason::nonMonotonicWallTime,
           "non-finite wall time must fail closed");
}

void TestLogicTimerAlpha() {
    double alpha = 0.0;
    double raw = 0.0;
    Expect(atb::clock::LogicTimerAlpha(110.0, 10.0, 1.0, 105.0, 0.0,
                                      alpha, &raw) &&
               std::abs(alpha - 0.5) < 1.0e-10 &&
               std::abs(raw - 0.5) < 1.0e-10,
           "logic-timer midpoint should produce alpha 0.5");
    Expect(!atb::clock::LogicTimerAlpha(110.0, 0.0, 1.0, 105.0, 0.0,
                                       alpha),
           "invalid logic step must fail closed");
    Expect(atb::clock::LogicTimerAlphaFromCompletedTick(
               100.0, 10.0, 1.0, 105.0, 0.0, alpha, &raw) &&
               std::abs(alpha - 0.5) < 1.0e-10 &&
               std::abs(raw - 0.5) < 1.0e-10,
           "completed-tick midpoint should produce alpha 0.5");
    Expect(atb::clock::LogicTimerAlphaFromCompletedTick(
               100.0, 10.0, 2.0, 102.5, 0.0, alpha, &raw) &&
               std::abs(alpha - 0.5) < 1.0e-10,
           "completed-tick alpha should honor timer speed");

    const auto interpolated = atb::clock::LogicTimerPresentationPhase(
        110.0, 10.0, 1.0, 105.0, 0.0);
    Expect(interpolated.kind == atb::clock::LogicPhaseKind::interpolated &&
               std::abs(interpolated.alpha - 0.5) < 1.0e-10,
           "presentation phase should interpolate an in-range residual");
    const auto overdue = atb::clock::LogicTimerPresentationPhase(
        110.0, 10.0, 1.0, 125.0, 0.0);
    Expect(overdue.kind == atb::clock::LogicPhaseKind::holdCurrent &&
               overdue.alpha == 1.0 && overdue.rawAlpha > 1.0,
           "overdue presentation must hold current state without extrapolation");
    const auto early = atb::clock::LogicTimerPresentationPhase(
        110.0, 10.0, 1.0, 96.0, 0.0);
    Expect(early.kind == atb::clock::LogicPhaseKind::unavailable,
           "implausibly early scheduler phase must fail closed");
}

} // namespace

int main() {
    for (const unsigned int gameplayRate : {30U, 60U, 120U}) {
        for (const unsigned int presentationRate : {144U, 165U, 239U, 240U, 241U, 360U}) {
            TestCadence(gameplayRate, presentationRate);
        }
    }
    TestConfigurationAndResetPaths();
    TestLogicTimerAlpha();

    if (g_failures != 0) {
        std::cerr << g_failures << " presentation-clock assertion(s) failed\n";
        return 1;
    }
    std::cout << "runtime-rate clock cadence and reset tests passed\n";
    return 0;
}
