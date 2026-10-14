#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace atb::clock {

enum class ResetReason : std::uint32_t {
    none,
    explicitDiscontinuity,
    invalidConfiguration,
    nonMonotonicWallTime,
    nonMonotonicTick,
    missingSnapshots,
    longStall,
};

enum class SampleKind : std::uint32_t {
    unavailable,
    interpolated,
    reset,
};

enum class LogicPhaseKind : std::uint32_t {
    unavailable,
    interpolated,
    holdCurrent,
};

struct LogicPhase {
    LogicPhaseKind kind = LogicPhaseKind::unavailable;
    double alpha = 0.0;
    double rawAlpha = 0.0;
};

struct PublishResult {
    bool accepted = false;
    ResetReason resetReason = ResetReason::none;
};

struct Sample {
    SampleKind kind = SampleKind::unavailable;
    ResetReason resetReason = ResetReason::none;
    std::uint64_t generation = 0;
    std::uint64_t olderSerial = 0;
    std::uint64_t newerSerial = 0;
    double alpha = 0.0;
    double targetTime = 0.0;
};

class PresentationClock final {
public:
    static constexpr std::size_t kHistoryCapacity = 2;
    static constexpr double kDefaultLongStallSeconds = 0.250;

    [[nodiscard]] bool Configure(
        double nativeStepSeconds,
        double longStallSeconds = kDefaultLongStallSeconds) noexcept;
    [[nodiscard]] PublishResult Publish(std::uint64_t serial,
                                        double wallTime,
                                        bool discontinuity = false) noexcept;
    [[nodiscard]] Sample Evaluate(double wallTime,
                                  double interpolationAlpha,
                                  bool phaseValid = true) noexcept;
    void Reset(ResetReason reason = ResetReason::explicitDiscontinuity) noexcept;

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    [[nodiscard]] double NativeStepSeconds() const noexcept { return nativeStepSeconds_; }
    [[nodiscard]] std::size_t SnapshotCount() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    [[nodiscard]] ResetReason LastResetReason() const noexcept { return lastResetReason_; }

private:
    struct Snapshot {
        std::uint64_t serial = 0;
        double wallArrivalTime = 0.0;
    };

    void Clear(ResetReason reason) noexcept;
    void Append(std::uint64_t serial, double wallTime) noexcept;
    [[nodiscard]] const Snapshot& At(std::size_t logicalIndex) const noexcept;

    std::array<Snapshot, kHistoryCapacity> snapshots_{};
    std::size_t first_ = 0;
    std::size_t count_ = 0;
    std::uint64_t generation_ = 0;
    ResetReason lastResetReason_ = ResetReason::none;
    double nativeStepSeconds_ = 0.0;
    double longStallSeconds_ = kDefaultLongStallSeconds;
    double lastSampleWallTime_ = 0.0;
    bool configured_ = false;
    bool hasLastSample_ = false;
};

[[nodiscard]] bool LogicTimerAlpha(double nextTickMilliseconds,
                                   double stepMilliseconds,
                                   double speedMultiplier,
                                   double nowMilliseconds,
                                   double presentationDelayTicks,
                                   double& alpha,
                                   double* rawAlpha = nullptr) noexcept;
[[nodiscard]] bool LogicTimerAlphaFromCompletedTick(
    double completedTickMilliseconds,
    double stepMilliseconds,
    double speedMultiplier,
    double nowMilliseconds,
    double presentationDelayTicks,
    double& alpha,
    double* rawAlpha = nullptr) noexcept;
[[nodiscard]] LogicPhase LogicTimerPresentationPhase(
    double nextTickMilliseconds,
    double stepMilliseconds,
    double speedMultiplier,
    double nowMilliseconds,
    double presentationDelayTicks) noexcept;
[[nodiscard]] const wchar_t* ResetReasonName(ResetReason reason) noexcept;
[[nodiscard]] const wchar_t* SampleKindName(SampleKind kind) noexcept;

} // namespace atb::clock
