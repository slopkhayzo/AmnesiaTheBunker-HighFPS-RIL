#include "hands_pose_interpolation.h"

#include <cmath>
#include <cstring>

namespace atb::hands_pose {
namespace {

float DotReal(const DualQuaternion& left,
              const DualQuaternion& right) noexcept {
    return left.values[0] * right.values[0] +
           left.values[1] * right.values[1] +
           left.values[2] * right.values[2] +
           left.values[3] * right.values[3];
}

float RealLengthSquared(const DualQuaternion& value) noexcept {
    return DotReal(value, value);
}

std::array<float, 3> Translation(
    const DualQuaternion& value) noexcept {
    const auto& q = value.values;
    const float inverseLengthSquared = 1.0F / RealLengthSquared(value);
    // Bunker stores real x/y/z/w followed by
    // 0.5 * translation_quaternion * real.
    return {
        2.0F * inverseLengthSquared *
            (-q[7] * q[0] + q[4] * q[3] - q[5] * q[2] + q[6] * q[1]),
        2.0F * inverseLengthSquared *
            (-q[7] * q[1] + q[4] * q[2] + q[5] * q[3] - q[6] * q[0]),
        2.0F * inverseLengthSquared *
            (-q[7] * q[2] - q[4] * q[1] + q[5] * q[0] + q[6] * q[3]),
    };
}

bool IsDiscontinuous(const Pose& older,
                     const Pose& newer,
                     const std::uint64_t serialDelta) noexcept {
    const float maximumTranslation =
        5.0F * static_cast<float>(serialDelta);
    const float maximumTranslationSquared =
        maximumTranslation * maximumTranslation;
    constexpr float minimumRotationDot = 0.5F;
    for (std::size_t bone = 0; bone < older.boneCount; ++bone) {
        const float lengthProduct = std::sqrt(
            RealLengthSquared(older.bones[bone]) *
            RealLengthSquared(newer.bones[bone]));
        if (!std::isfinite(lengthProduct) || lengthProduct < 1.0e-8F ||
            std::abs(DotReal(older.bones[bone], newer.bones[bone])) /
                    lengthProduct <
                minimumRotationDot) {
            return true;
        }
        const auto first = Translation(older.bones[bone]);
        const auto second = Translation(newer.bones[bone]);
        const float dx = second[0] - first[0];
        const float dy = second[1] - first[1];
        const float dz = second[2] - first[2];
        if (dx * dx + dy * dy + dz * dz >
            maximumTranslationSquared) {
            return true;
        }
    }
    return false;
}

bool InterpolateDualQuaternion(const DualQuaternion& older,
                               const DualQuaternion& newer,
                               const float alpha,
                               DualQuaternion& result) noexcept {
    const float sign = DotReal(older, newer) < 0.0F ? -1.0F : 1.0F;
    for (std::size_t index = 0; index < result.values.size(); ++index) {
        const float endpoint = newer.values[index] * sign;
        result.values[index] = older.values[index] +
            (endpoint - older.values[index]) * alpha;
    }
    const float lengthSquared = RealLengthSquared(result);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8F) {
        return false;
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    for (float& value : result.values) {
        value *= inverseLength;
    }
    const float dualDot =
        result.values[0] * result.values[4] +
        result.values[1] * result.values[5] +
        result.values[2] * result.values[6] +
        result.values[3] * result.values[7];
    for (std::size_t index = 0; index < 4; ++index) {
        result.values[index + 4] -= result.values[index] * dualDot;
    }
    return IsValid(result);
}

} // namespace

bool IsValid(const DualQuaternion& dualQuaternion) noexcept {
    for (const float value : dualQuaternion.values) {
        if (!std::isfinite(value) || std::abs(value) > 10000.0F) {
            return false;
        }
    }
    const float lengthSquared = RealLengthSquared(dualQuaternion);
    return std::isfinite(lengthSquared) && lengthSquared > 0.25F &&
           lengthSquared < 4.0F;
}

bool IsValidPalette(const DualQuaternion* const palette,
                    const std::size_t count) noexcept {
    if (palette == nullptr || count == 0 || count > kMaximumBones) {
        return false;
    }
    for (std::size_t bone = 0; bone < count; ++bone) {
        if (!IsValid(palette[bone])) {
            return false;
        }
    }
    return true;
}

bool IsDiscontinuousPalette(const DualQuaternion* const older,
                            const DualQuaternion* const newer,
                            const std::size_t count) noexcept {
    constexpr float maximumTranslationSquared = 5.0F * 5.0F;
    constexpr float minimumRotationDot = 0.5F;
    for (std::size_t bone = 0; bone < count; ++bone) {
        const float lengthProduct = std::sqrt(
            RealLengthSquared(older[bone]) * RealLengthSquared(newer[bone]));
        if (!std::isfinite(lengthProduct) || lengthProduct < 1.0e-8F ||
            std::abs(DotReal(older[bone], newer[bone])) / lengthProduct <
                minimumRotationDot) {
            return true;
        }
        const auto first = Translation(older[bone]);
        const auto second = Translation(newer[bone]);
        const float dx = second[0] - first[0];
        const float dy = second[1] - first[1];
        const float dz = second[2] - first[2];
        if (dx * dx + dy * dy + dz * dz > maximumTranslationSquared) {
            return true;
        }
    }
    return false;
}

bool InterpolatePalette(const DualQuaternion* const older,
                        const DualQuaternion* const newer,
                        const std::size_t count,
                        const double alpha,
                        DualQuaternion* const result) noexcept {
    if (older == nullptr || newer == nullptr || result == nullptr ||
        count == 0 || count > kMaximumBones || !std::isfinite(alpha) ||
        alpha < 0.0 || alpha > 1.0) {
        return false;
    }
    const float t = static_cast<float>(alpha);
    for (std::size_t bone = 0; bone < count; ++bone) {
        if (!InterpolateDualQuaternion(older[bone], newer[bone], t,
                                       result[bone])) {
            return false;
        }
    }
    return true;
}

bool BlendPalettePerBone(const DualQuaternion* const older,
                         const DualQuaternion* const newer,
                         const std::size_t count,
                         const double alpha,
                         DualQuaternion* const result,
                         std::size_t& snappedBones) noexcept {
    snappedBones = 0;
    if (older == nullptr || newer == nullptr || result == nullptr ||
        count == 0 || count > kMaximumBones || !std::isfinite(alpha) ||
        alpha < 0.0 || alpha > 1.0) {
        return false;
    }
    constexpr float minimumLength = 1.0e-6F;
    constexpr float minimumRotationDot = 0.5F;
    constexpr float maximumTranslationSquared = 5.0F * 5.0F;
    const float t = static_cast<float>(alpha);
    std::size_t blended = 0;
    for (std::size_t bone = 0; bone < count; ++bone) {
        const DualQuaternion& from = older[bone];
        const DualQuaternion& to = newer[bone];
        for (std::size_t index = 0; index < 8; ++index) {
            if (!std::isfinite(from.values[index]) ||
                !std::isfinite(to.values[index])) {
                return false;
            }
        }
        result[bone] = to;
        const float fromLength = std::sqrt(RealLengthSquared(from));
        const float toLength = std::sqrt(RealLengthSquared(to));
        if (!(fromLength > minimumLength) || !(toLength > minimumLength)) {
            continue; // unused bone: nothing to blend
        }
        const auto first = Translation(from);
        const auto second = Translation(to);
        const float dx = second[0] - first[0];
        const float dy = second[1] - first[1];
        const float dz = second[2] - first[2];
        if (std::abs(DotReal(from, to)) / (fromLength * toLength) <
                minimumRotationDot ||
            !(dx * dx + dy * dy + dz * dz <= maximumTranslationSquared)) {
            ++snappedBones;
            continue;
        }
        DualQuaternion fromUnit = from;
        DualQuaternion toUnit = to;
        for (std::size_t index = 0; index < 8; ++index) {
            fromUnit.values[index] /= fromLength;
            toUnit.values[index] /= toLength;
        }
        DualQuaternion unit{};
        if (!InterpolateDualQuaternion(fromUnit, toUnit, t, unit)) {
            ++snappedBones;
            continue;
        }
        const float magnitude = fromLength + (toLength - fromLength) * t;
        for (std::size_t index = 0; index < 8; ++index) {
            result[bone].values[index] = unit.values[index] * magnitude;
        }
        ++blended;
    }
    return blended != 0;
}

bool IsValid(const Pose& pose) noexcept {
    if (pose.boneCount == 0 || pose.boneCount > kMaximumBones) {
        return false;
    }
    for (std::size_t bone = 0; bone < pose.boneCount; ++bone) {
        if (!IsValid(pose.bones[bone])) {
            return false;
        }
    }
    return true;
}

bool NearlyEqual(const Pose& left, const Pose& right) noexcept {
    if (left.boneCount != right.boneCount) {
        return false;
    }
    const std::size_t bytes = left.boneCount * sizeof(DualQuaternion);
    return std::memcmp(left.bones.data(), right.bones.data(), bytes) == 0;
}

bool Interpolate(const Pose& older,
                 const Pose& newer,
                 const double alpha,
                 Pose& result) noexcept {
    result = {};
    if (!IsValid(older) || !IsValid(newer) ||
        older.boneCount != newer.boneCount || !std::isfinite(alpha) ||
        alpha < 0.0 || alpha > 1.0) {
        return false;
    }
    const float t = static_cast<float>(alpha);
    result.boneCount = older.boneCount;
    for (std::size_t bone = 0; bone < result.boneCount; ++bone) {
        if (!InterpolateDualQuaternion(older.bones[bone], newer.bones[bone],
                                       t, result.bones[bone])) {
            result = {};
            return false;
        }
    }
    return true;
}

void History::Reset() noexcept {
    count_ = 0;
    generation_ = 0;
    hasGeneration_ = false;
}

PublishKind History::Publish(const std::uint64_t serial,
                             const std::uint64_t generation,
                             const Pose& pose) noexcept {
    if (!IsValid(pose)) {
        Reset();
        return PublishKind::invalid;
    }
    if (!hasGeneration_ || generation_ != generation) {
        Reset();
        generation_ = generation;
        hasGeneration_ = true;
        snapshots_[0] = {serial, pose};
        count_ = 1;
        return PublishKind::resetGeneration;
    }
    if (count_ != 0) {
        const Snapshot& latest = snapshots_[count_ - 1];
        if (serial < latest.serial) {
            Reset();
            generation_ = generation;
            hasGeneration_ = true;
            snapshots_[0] = {serial, pose};
            count_ = 1;
            return PublishKind::resetNonMonotonic;
        }
        if (serial == latest.serial) {
            if (NearlyEqual(latest.pose, pose)) {
                return PublishKind::unchanged;
            }
            Reset();
            generation_ = generation;
            hasGeneration_ = true;
            snapshots_[0] = {serial, pose};
            count_ = 1;
            return PublishKind::resetSameTickMutation;
        }
        if (latest.pose.boneCount != pose.boneCount) {
            Reset();
            generation_ = generation;
            hasGeneration_ = true;
            snapshots_[0] = {serial, pose};
            count_ = 1;
            return PublishKind::resetBoneCount;
        }
        const std::uint64_t delta = serial - latest.serial;
        if (delta > 2 || IsDiscontinuous(latest.pose, pose, delta)) {
            Reset();
            generation_ = generation;
            hasGeneration_ = true;
            snapshots_[0] = {serial, pose};
            count_ = 1;
            return PublishKind::resetDiscontinuity;
        }
    }
    if (count_ == 0) {
        snapshots_[0] = {serial, pose};
        count_ = 1;
    } else if (count_ == 1) {
        snapshots_[1] = {serial, pose};
        count_ = 2;
    } else {
        snapshots_[0] = snapshots_[1];
        snapshots_[1] = {serial, pose};
    }
    return PublishKind::appended;
}

SampleResult History::Evaluate(const clock::Sample& sample) const noexcept {
    if (sample.kind != clock::SampleKind::interpolated ||
        sample.olderSerial == sample.newerSerial) {
        return {SampleKind::clockUnavailable, {}};
    }
    if (!hasGeneration_ || generation_ != sample.generation) {
        return {SampleKind::generationMismatch, {}};
    }
    const Snapshot* older = nullptr;
    const Snapshot* newer = nullptr;
    for (std::size_t index = 0; index < count_; ++index) {
        if (snapshots_[index].serial == sample.olderSerial) {
            older = &snapshots_[index];
        }
        if (snapshots_[index].serial == sample.newerSerial) {
            newer = &snapshots_[index];
        }
    }
    if (older == nullptr || newer == nullptr) {
        return {SampleKind::snapshotsMissing, {}};
    }
    SampleResult result{SampleKind::applied, {}};
    if (!Interpolate(older->pose, newer->pose, sample.alpha, result.pose)) {
        return {SampleKind::invalid, {}};
    }
    return result;
}

} // namespace atb::hands_pose
