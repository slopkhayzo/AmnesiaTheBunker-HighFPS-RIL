#include "camera_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

constexpr float kPi = 3.14159265358979323846F;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

atb::camera::Pose MakePose(const float x,
                           const float pitch = 0.0F,
                           const float yaw = 0.0F,
                           const float roll = 0.0F) {
    return {{x, x * 2.0F, -x}, pitch, yaw, roll};
}

atb::clock::Sample Pair(const std::uint64_t generation,
                        const std::uint64_t older,
                        const std::uint64_t newer,
                        const double alpha) {
    atb::clock::Sample sample{};
    sample.kind = atb::clock::SampleKind::interpolated;
    sample.generation = generation;
    sample.olderSerial = older;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

void TestLinearPositionAndYxzOrientation() {
    const auto older = MakePose(0.0F, -0.2F, 0.3F, -0.1F);
    const auto newer = MakePose(2.0F, 0.2F, 0.7F, 0.1F);
    atb::camera::Pose midpoint{};
    Require(atb::camera::Interpolate(older, newer, 0.5, midpoint),
            "valid midpoint interpolation was rejected");
    Require(std::abs(midpoint.position.x - 1.0F) < 1.0e-5F &&
                std::abs(midpoint.position.y - 2.0F) < 1.0e-5F &&
                std::abs(midpoint.position.z + 1.0F) < 1.0e-5F,
            "position was not interpolated linearly");
    Require(atb::camera::OrientationDistance(older, midpoint) > 0.0F &&
                atb::camera::OrientationDistance(midpoint, newer) > 0.0F,
            "YXZ orientation midpoint did not lie between endpoints");

    atb::camera::Pose endpoint{};
    Require(atb::camera::Interpolate(older, newer, 0.0, endpoint) &&
                atb::camera::OrientationDistance(endpoint, older) < 1.0e-4F,
            "quaternion conversion changed the older endpoint");
    Require(atb::camera::Interpolate(older, newer, 1.0, endpoint) &&
                atb::camera::OrientationDistance(endpoint, newer) < 1.0e-4F,
            "quaternion conversion changed the newer endpoint");
}

void TestShortestArcAcrossYawWrap() {
    const auto older = MakePose(0.0F, 0.0F, 179.0F * kPi / 180.0F);
    const auto newer = MakePose(0.0F, 0.0F, -179.0F * kPi / 180.0F);
    atb::camera::Pose midpoint{};
    Require(atb::camera::Interpolate(older, newer, 0.5, midpoint),
            "yaw-wrap interpolation was rejected");
    const auto expected = MakePose(0.0F, 0.0F, kPi);
    Require(atb::camera::OrientationDistance(midpoint, expected) < 1.0e-3F,
            "yaw wrap took the long arc through zero");
}

void TestHistoryPairingAndGeneration() {
    atb::camera::History history;
    Require(history.Publish(10, 4, MakePose(0.0F)).kind ==
                atb::camera::PublishKind::resetGeneration,
            "first pose did not establish the clock generation");
    Require(history.Publish(11, 4, MakePose(1.0F)).kind ==
                atb::camera::PublishKind::appended,
            "second pose was not appended");
    const auto sampled = history.Evaluate(Pair(4, 10, 11, 0.25));
    Require(sampled.kind == atb::camera::SampleKind::applied,
            "exact serial pair was not sampled");
    Require(std::abs(sampled.pose.position.x - 0.25F) < 1.0e-5F,
            "history used the wrong interpolation alpha");
    Require(history.Evaluate(Pair(5, 10, 11, 0.5)).kind ==
                atb::camera::SampleKind::generationMismatch,
            "history crossed a clock-generation reset");
}

void TestMutationGapAndDiscontinuityReset() {
    atb::camera::History history;
    (void)history.Publish(20, 2, MakePose(0.0F));
    Require(history.Publish(20, 2, MakePose(0.1F)).kind ==
                atb::camera::PublishKind::resetSameTickMutation,
            "same-tick camera mutation did not reset history");
    Require(history.SnapshotCount() == 1,
            "same-tick reset did not retain only the current endpoint");
    Require(history.Publish(22, 2, MakePose(0.2F)).kind ==
                atb::camera::PublishKind::resetDiscontinuity,
            "a missing camera snapshot did not reset history");
    Require(history.Publish(23, 2, MakePose(20.0F)).kind ==
                atb::camera::PublishKind::resetDiscontinuity,
            "a large camera teleport was interpolated");
    Require(history.Evaluate(Pair(2, 22, 23, 0.5)).kind ==
                atb::camera::SampleKind::snapshotsMissing,
            "history crossed a teleport reset");
}

void TestInvalidInputFailsClosed() {
    auto invalid = MakePose(0.0F);
    invalid.yaw = std::numeric_limits<float>::quiet_NaN();
    atb::camera::Pose output{};
    Require(!atb::camera::Interpolate(invalid, MakePose(1.0F), 0.5, output),
            "non-finite orientation was accepted");
    Require(!atb::camera::Interpolate(MakePose(0.0F), MakePose(1.0F), 1.1,
                                      output),
            "out-of-range alpha was accepted");

    atb::camera::History history;
    Require(history.Publish(1, 1, invalid).kind ==
                atb::camera::PublishKind::invalid &&
                history.SnapshotCount() == 0,
            "invalid pose did not clear history");
}

} // namespace

int main() {
    TestLinearPositionAndYxzOrientation();
    TestShortestArcAcrossYawWrap();
    TestHistoryPairingAndGeneration();
    TestMutationGapAndDiscontinuityReset();
    TestInvalidInputFailsClosed();
    std::cout << "camera interpolation tests passed\n";
    return EXIT_SUCCESS;
}
