#pragma once

#include "presentation_clock.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace atb::hands_pose {

constexpr std::size_t kMaximumBones = 256;

struct DualQuaternion {
    std::array<float, 8> values{};
};

static_assert(sizeof(DualQuaternion) == 32);

struct Pose {
    std::array<DualQuaternion, kMaximumBones> bones{};
    std::size_t boneCount = 0;
};

enum class PublishKind : std::uint8_t {
    appended,
    unchanged,
    invalid,
    resetGeneration,
    resetNonMonotonic,
    resetSameTickMutation,
    resetBoneCount,
    resetDiscontinuity,
};

enum class SampleKind : std::uint8_t {
    applied,
    clockUnavailable,
    generationMismatch,
    snapshotsMissing,
    invalid,
};

struct SampleResult {
    SampleKind kind = SampleKind::clockUnavailable;
    Pose pose{};
};

class History final {
public:
    void Reset() noexcept;
    [[nodiscard]] PublishKind Publish(std::uint64_t serial,
                                      std::uint64_t generation,
                                      const Pose& pose) noexcept;
    [[nodiscard]] SampleResult Evaluate(
        const clock::Sample& sample) const noexcept;

private:
    struct Snapshot {
        std::uint64_t serial = 0;
        Pose pose{};
    };

    std::array<Snapshot, 2> snapshots_{};
    std::size_t count_ = 0;
    std::uint64_t generation_ = 0;
    bool hasGeneration_ = false;
};

[[nodiscard]] bool IsValid(
    const DualQuaternion& dualQuaternion) noexcept;
[[nodiscard]] bool IsValid(const Pose& pose) noexcept;
[[nodiscard]] bool NearlyEqual(const Pose& left,
                               const Pose& right) noexcept;
// Palette-span forms of the same checks and blend, for callers that keep
// their own storage.
[[nodiscard]] bool IsValidPalette(const DualQuaternion* palette,
                                  std::size_t count) noexcept;
[[nodiscard]] bool IsDiscontinuousPalette(const DualQuaternion* older,
                                          const DualQuaternion* newer,
                                          std::size_t count) noexcept;
[[nodiscard]] bool InterpolatePalette(const DualQuaternion* older,
                                      const DualQuaternion* newer,
                                      std::size_t count,
                                      double alpha,
                                      DualQuaternion* result) noexcept;
// Per-bone blend for arbitrary world skeletons. Real skeletons carry bones
// the strict checks above reject as a whole palette: unused bones with a
// zero quaternion, and bones whose quaternion is not unit length. Here each
// bone is judged on its own. A degenerate bone, or one that turns more than
// about 120 degrees or moves more than 5 m in one tick, takes the newer pose
// unblended; every other bone is blended and keeps the interpolated
// magnitude of its endpoints. Returns false, leaving `result` unspecified,
// if any value is non-finite or no bone could be blended (a full cut).
[[nodiscard]] bool BlendPalettePerBone(const DualQuaternion* older,
                                       const DualQuaternion* newer,
                                       std::size_t count,
                                       double alpha,
                                       DualQuaternion* result,
                                       std::size_t& snappedBones) noexcept;
[[nodiscard]] bool Interpolate(const Pose& older,
                               const Pose& newer,
                               double alpha,
                               Pose& result) noexcept;

} // namespace atb::hands_pose
