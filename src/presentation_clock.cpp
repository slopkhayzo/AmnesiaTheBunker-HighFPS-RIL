#include "presentation_clock.h"

#include <algorithm>
#include <cmath>

namespace atb::clock {
namespace {

constexpr double kTimeEpsilon = 1.0e-9;

} // namespace

bool PresentationClock::Configure(const double nativeStepSeconds,
                                  const double longStallSeconds) noexcept {
    if (!std::isfinite(nativeStepSeconds) ||
        !std::isfinite(longStallSeconds) || nativeStepSeconds < 0.001 ||
        nativeStepSeconds > 1.0 || longStallSeconds < nativeStepSeconds ||
        longStallSeconds > 10.0) {
        configured_ = false;
        nativeStepSeconds_ = 0.0;
        Clear(ResetReason::invalidConfiguration);
        return false;
    }
    nativeStepSeconds_ = nativeStepSeconds;
    longStallSeconds_ = longStallSeconds;
    configured_ = true;
    Clear(ResetReason::none);
    return true;
}

void PresentationClock::Clear(const ResetReason reason) noexcept {
    first_ = 0;
    count_ = 0;
    hasLastSample_ = false;
    lastResetReason_ = reason;
    ++generation_;
}

void PresentationClock::Reset(const ResetReason reason) noexcept { Clear(reason); }

const PresentationClock::Snapshot&
PresentationClock::At(const std::size_t logicalIndex) const noexcept {
    return snapshots_[(first_ + logicalIndex) % kHistoryCapacity];
}

void PresentationClock::Append(const std::uint64_t serial,
                               const double wallTime) noexcept {
    const Snapshot snapshot{serial, wallTime};
    if (count_ < kHistoryCapacity) {
        snapshots_[(first_ + count_) % kHistoryCapacity] = snapshot;
        ++count_;
    } else {
        snapshots_[first_] = snapshot;
        first_ = (first_ + 1) % kHistoryCapacity;
    }
}

PublishResult PresentationClock::Publish(const std::uint64_t serial,
                                         const double wallTime,
                                         const bool discontinuity) noexcept {
    if (!configured_) {
        return {false, ResetReason::invalidConfiguration};
    }
    ResetReason resetReason = ResetReason::none;
    if (!std::isfinite(wallTime)) {
        Clear(ResetReason::nonMonotonicWallTime);
        return {false, ResetReason::nonMonotonicWallTime};
    }
    if (discontinuity) {
        Clear(ResetReason::explicitDiscontinuity);
        resetReason = ResetReason::explicitDiscontinuity;
    } else if (count_ != 0) {
        const Snapshot& latest = At(count_ - 1);
        if (serial <= latest.serial) {
            Clear(ResetReason::nonMonotonicTick);
            resetReason = ResetReason::nonMonotonicTick;
        } else if (serial - latest.serial != 1U) {
            Clear(ResetReason::missingSnapshots);
            resetReason = ResetReason::missingSnapshots;
        } else if (wallTime + kTimeEpsilon < latest.wallArrivalTime) {
            Clear(ResetReason::nonMonotonicWallTime);
            resetReason = ResetReason::nonMonotonicWallTime;
        } else if (wallTime - latest.wallArrivalTime > longStallSeconds_) {
            Clear(ResetReason::longStall);
            resetReason = ResetReason::longStall;
        }
    }
    Append(serial, wallTime);
    return {true, resetReason};
}

Sample PresentationClock::Evaluate(const double wallTime,
                                   const double interpolationAlpha,
                                   const bool phaseValid) noexcept {
    Sample result{};
    result.generation = generation_;
    if (!configured_) {
        result.resetReason = ResetReason::invalidConfiguration;
        return result;
    }
    if (!std::isfinite(wallTime)) {
        Clear(ResetReason::nonMonotonicWallTime);
        result.kind = SampleKind::reset;
        result.resetReason = ResetReason::nonMonotonicWallTime;
        result.generation = generation_;
        return result;
    }
    if (hasLastSample_) {
        if (wallTime + kTimeEpsilon < lastSampleWallTime_) {
            Clear(ResetReason::nonMonotonicWallTime);
            result.kind = SampleKind::reset;
            result.resetReason = ResetReason::nonMonotonicWallTime;
            result.generation = generation_;
            return result;
        }
        if (wallTime - lastSampleWallTime_ > longStallSeconds_) {
            Clear(ResetReason::longStall);
            result.kind = SampleKind::reset;
            result.resetReason = ResetReason::longStall;
            result.generation = generation_;
            return result;
        }
    }
    lastSampleWallTime_ = wallTime;
    hasLastSample_ = true;

    if (count_ == 0) {
        return result;
    }
    const Snapshot& latest = At(count_ - 1);
    if (wallTime - latest.wallArrivalTime > longStallSeconds_) {
        Clear(ResetReason::longStall);
        result.kind = SampleKind::reset;
        result.resetReason = ResetReason::longStall;
        result.generation = generation_;
        return result;
    }
    if (count_ < kHistoryCapacity || !phaseValid ||
        !std::isfinite(interpolationAlpha)) {
        return result;
    }
    const Snapshot& oldest = At(0);
    if (latest.serial != oldest.serial + 1U) {
        Clear(ResetReason::missingSnapshots);
        result.kind = SampleKind::reset;
        result.resetReason = ResetReason::missingSnapshots;
        result.generation = generation_;
        return result;
    }
    result.kind = SampleKind::interpolated;
    result.olderSerial = oldest.serial;
    result.newerSerial = latest.serial;
    result.alpha = std::clamp(interpolationAlpha, 0.0, 1.0);
    result.targetTime =
        (static_cast<double>(oldest.serial) + result.alpha) * nativeStepSeconds_;
    return result;
}

bool LogicTimerAlpha(const double nextTickMilliseconds,
                     const double stepMilliseconds,
                     const double speedMultiplier,
                     const double nowMilliseconds,
                     const double presentationDelayTicks,
                     double& alpha,
                     double* const rawAlphaOutput) noexcept {
    alpha = 0.0;
    if (rawAlphaOutput != nullptr) {
        *rawAlphaOutput = 0.0;
    }
    if (!std::isfinite(nextTickMilliseconds) ||
        !std::isfinite(stepMilliseconds) ||
        !std::isfinite(speedMultiplier) || !std::isfinite(nowMilliseconds) ||
        !std::isfinite(presentationDelayTicks) || stepMilliseconds < 1.0 ||
        stepMilliseconds > 1000.0 || speedMultiplier < 0.01 ||
        speedMultiplier > 100.0 || presentationDelayTicks < 0.0 ||
        presentationDelayTicks > 1.0) {
        return false;
    }
    const double interval = stepMilliseconds / speedMultiplier;
    const double rawAlpha =
        (nowMilliseconds - (nextTickMilliseconds - interval)) / interval;
    if (rawAlphaOutput != nullptr) {
        *rawAlphaOutput = rawAlpha;
    }
    const double delayedAlpha = rawAlpha - presentationDelayTicks;
    if (!std::isfinite(rawAlpha) || !std::isfinite(delayedAlpha) ||
        rawAlpha < -0.25 || rawAlpha > 2.25 || delayedAlpha < -0.25 ||
        delayedAlpha > 1.25) {
        return false;
    }
    alpha = std::clamp(delayedAlpha, 0.0, 1.0);
    return true;
}

bool LogicTimerAlphaFromCompletedTick(
    const double completedTickMilliseconds,
    const double stepMilliseconds,
    const double speedMultiplier,
    const double nowMilliseconds,
    const double presentationDelayTicks,
    double& alpha,
    double* const rawAlphaOutput) noexcept {
    if (!std::isfinite(completedTickMilliseconds) ||
        !std::isfinite(stepMilliseconds) ||
        !std::isfinite(speedMultiplier) || speedMultiplier < 0.01 ||
        speedMultiplier > 100.0) {
        alpha = 0.0;
        if (rawAlphaOutput != nullptr) {
            *rawAlphaOutput = 0.0;
        }
        return false;
    }
    const double nextTickMilliseconds =
        completedTickMilliseconds + stepMilliseconds / speedMultiplier;
    return LogicTimerAlpha(nextTickMilliseconds, stepMilliseconds,
                           speedMultiplier, nowMilliseconds,
                           presentationDelayTicks, alpha, rawAlphaOutput);
}

LogicPhase LogicTimerPresentationPhase(
    const double nextTickMilliseconds,
    const double stepMilliseconds,
    const double speedMultiplier,
    const double nowMilliseconds,
    const double presentationDelayTicks) noexcept {
    LogicPhase result{};
    if (!std::isfinite(nextTickMilliseconds) ||
        !std::isfinite(stepMilliseconds) ||
        !std::isfinite(speedMultiplier) || !std::isfinite(nowMilliseconds) ||
        !std::isfinite(presentationDelayTicks) || stepMilliseconds < 1.0 ||
        stepMilliseconds > 1000.0 || speedMultiplier < 0.01 ||
        speedMultiplier > 100.0 || presentationDelayTicks < 0.0 ||
        presentationDelayTicks > 1.0) {
        return result;
    }
    const double interval = stepMilliseconds / speedMultiplier;
    result.rawAlpha =
        (nowMilliseconds - (nextTickMilliseconds - interval)) / interval -
        presentationDelayTicks;
    if (!std::isfinite(result.rawAlpha) || result.rawAlpha < -0.25) {
        return result;
    }
    result.alpha = std::clamp(result.rawAlpha, 0.0, 1.0);
    result.kind = result.rawAlpha > 1.0
        ? LogicPhaseKind::holdCurrent
        : LogicPhaseKind::interpolated;
    return result;
}

const wchar_t* ResetReasonName(const ResetReason reason) noexcept {
    switch (reason) {
    case ResetReason::none: return L"none";
    case ResetReason::explicitDiscontinuity: return L"explicit-discontinuity";
    case ResetReason::invalidConfiguration: return L"invalid-configuration";
    case ResetReason::nonMonotonicWallTime: return L"non-monotonic-wall-time";
    case ResetReason::nonMonotonicTick: return L"non-monotonic-tick";
    case ResetReason::missingSnapshots: return L"missing-snapshots";
    case ResetReason::longStall: return L"long-stall";
    }
    return L"unknown";
}

const wchar_t* SampleKindName(const SampleKind kind) noexcept {
    switch (kind) {
    case SampleKind::unavailable: return L"unavailable";
    case SampleKind::interpolated: return L"interpolated";
    case SampleKind::reset: return L"reset";
    }
    return L"unknown";
}

} // namespace atb::clock
