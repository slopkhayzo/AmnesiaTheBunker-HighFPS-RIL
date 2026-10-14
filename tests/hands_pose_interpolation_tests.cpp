#include "hands_pose_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

atb::hands_pose::Pose MakePose(const float translation,
                               const std::size_t bones = 23) {
    atb::hands_pose::Pose pose{};
    pose.boneCount = bones;
    for (std::size_t index = 0; index < bones; ++index) {
        auto& values = pose.bones[index].values;
        values[3] = 1.0F;
        values[4] = translation * 0.5F;
    }
    return pose;
}

} // namespace

int main() {
    const auto older = MakePose(0.0F);
    const auto newer = MakePose(2.0F);
    atb::hands_pose::Pose result{};
    Require(atb::hands_pose::Interpolate(older, newer, 0.5, result) &&
                result.boneCount == 23 &&
                std::abs(result.bones[0].values[4] - 0.5F) < 1.0e-5F,
            "valid dual-quaternion palette did not interpolate at midpoint");

    auto antipodal = newer;
    for (float& value : antipodal.bones[0].values) {
        value = -value;
    }
    Require(atb::hands_pose::Interpolate(older, antipodal, 0.5, result) &&
                std::abs(result.bones[0].values[3] - 1.0F) < 1.0e-5F &&
                std::abs(result.bones[0].values[4] - 0.5F) < 1.0e-5F,
            "antipodal dual quaternion did not use the shortest path");

    atb::hands_pose::History history{};
    Require(history.Publish(10, 3, older) ==
                atb::hands_pose::PublishKind::resetGeneration,
            "first palette did not establish its clock generation");
    Require(history.Publish(11, 3, newer) ==
                atb::hands_pose::PublishKind::appended,
            "second palette did not append");
    const atb::clock::Sample sample{
        atb::clock::SampleKind::interpolated,
        atb::clock::ResetReason::none,
        3,
        10,
        11,
        0.5,
        0.0,
    };
    const auto evaluated = history.Evaluate(sample);
    Require(evaluated.kind == atb::hands_pose::SampleKind::applied &&
                std::abs(evaluated.pose.bones[0].values[4] - 0.5F) < 1.0e-5F,
            "history did not evaluate the requested presentation pair");

    auto mutated = newer;
    mutated.bones[0].values[4] = 1.25F;
    Require(history.Publish(11, 3, mutated) ==
                atb::hands_pose::PublishKind::resetSameTickMutation,
            "same-tick palette mutation did not reset history");

    auto invalid = newer;
    invalid.bones[0].values[3] = 0.0F;
    Require(!atb::hands_pose::IsValid(invalid),
            "zero-length real quaternion was accepted");
    Require(!atb::hands_pose::Interpolate(older, newer, 1.1, result),
            "out-of-range alpha was accepted");

    std::cout << "hands dual-quaternion interpolation tests passed\n";
    return EXIT_SUCCESS;
}
