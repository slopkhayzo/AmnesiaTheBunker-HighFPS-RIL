#include "hands_trace.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

atb::hands_trace::Matrix ScaledIdentity(const float scale,
                                        const float x,
                                        const float y,
                                        const float z) {
    atb::hands_trace::Matrix matrix{};
    matrix.values[0] = scale;
    matrix.values[5] = scale;
    matrix.values[10] = scale;
    matrix.values[3] = x;
    matrix.values[7] = y;
    matrix.values[11] = z;
    matrix.values[15] = 1.0F;
    return matrix;
}

struct FakeEntity {
    void** vtable = nullptr;
    atb::hands_trace::Matrix matrix{};
    void* meshEntity = nullptr;
};

atb::hands_trace::Matrix* GetFakeMatrix(
    void* const rawEntity,
    atb::hands_trace::Matrix* const output) {
    const auto* const entity = static_cast<const FakeEntity*>(rawEntity);
    *output = entity->matrix;
    return output;
}

void SetFakeMatrix(void* const rawEntity,
                   const atb::hands_trace::Matrix* const matrix) {
    auto* const entity = static_cast<FakeEntity*>(rawEntity);
    entity->matrix = *matrix;
}

void* GetFakeMeshEntity(void* const rawEntity) {
    return static_cast<FakeEntity*>(rawEntity)->meshEntity;
}

} // namespace

int main() {
    const float camera[3] = {10.0F, 2.0F, -4.0F};
    const auto hands = ScaledIdentity(0.25F, 10.0F, 1.925F, -4.0F);
    Require(atb::hands_trace::IsFiniteAffine(hands),
            "ordinary hands matrix should be finite affine");
    Require(atb::hands_trace::IsOrdinaryHandsRoot(hands, camera),
            "quarter-scale near-camera matrix should identify ordinary hands");
    const auto handsScale = atb::hands_trace::BasisLengths(hands);
    Require(handsScale[0] == 0.25F && handsScale[1] == 0.25F &&
                handsScale[2] == 0.25F,
            "basis lengths should expose the uniform hands scale");
    Require(atb::hands_trace::HasUniformScale(hands, 0.25F) &&
                !atb::hands_trace::HasUniformScale(hands, 1.0F),
            "uniform-scale classifier should distinguish quarter and unit scale");
    Require(atb::hands_trace::DistanceFrom(hands, camera) > 0.07F &&
                atb::hands_trace::DistanceFrom(hands, camera) < 0.08F,
            "camera distance should use matrix translation");

    const auto fullScaleHands = ScaledIdentity(1.0F, 10.0F, 2.0F, -4.0F);
    Require(atb::hands_trace::IsOrdinaryHandsRoot(fullScaleHands, camera),
            "unit-scale matrix at the camera should identify Bunker hands");

    const auto world = ScaledIdentity(1.0F, 10.0F, 1.7F, -4.0F);
    Require(!atb::hands_trace::IsOrdinaryHandsRoot(world, camera),
            "offset unit-scale near-camera object must not identify as hands root");
    Require(atb::hands_trace::IsNearCamera(world, camera),
            "near-camera attached item should pass proximity classification");

    const auto distant = ScaledIdentity(0.25F, 30.0F, 2.0F, -4.0F);
    Require(!atb::hands_trace::IsOrdinaryHandsRoot(distant, camera),
            "distant quarter-scale object must not identify as hands root");

    auto invalid = hands;
    invalid.values[6] = std::numeric_limits<float>::quiet_NaN();
    Require(!atb::hands_trace::IsFiniteAffine(invalid),
            "non-finite matrix must fail closed");
    Require(atb::hands_trace::SameMatrix(hands, hands),
            "identical matrices should compare equal");
    Require(!atb::hands_trace::SameMatrix(hands, world),
            "different matrices should not compare equal");

    auto rotatedRoot = ScaledIdentity(1.0F, 3.0F, -2.0F, 5.0F);
    rotatedRoot.values[0] = 0.0F;
    rotatedRoot.values[1] = -1.0F;
    rotatedRoot.values[4] = 1.0F;
    rotatedRoot.values[5] = 0.0F;
    const auto local = ScaledIdentity(1.0F, 0.25F, 0.5F, -0.75F);
    const auto composed = atb::hands_trace::Multiply(rotatedRoot, local);
    atb::hands_trace::Matrix inverseRoot{};
    Require(atb::hands_trace::InvertAffine(rotatedRoot, inverseRoot),
            "finite rigid matrix should have an affine inverse");
    const auto recoveredLocal =
        atb::hands_trace::Multiply(inverseRoot, composed);
    Require(atb::hands_trace::MaximumAbsoluteDifference(recoveredLocal,
                                                        local) < 1.0e-6F,
            "inverse-root composition should recover the local transform");
    auto singular = rotatedRoot;
    singular.values[0] = singular.values[1] = singular.values[2] = 0.0F;
    Require(!atb::hands_trace::InvertAffine(singular, inverseRoot),
            "singular affine matrix must fail closed");

    auto olderRigid = ScaledIdentity(1.0F, 0.0F, 2.0F, 4.0F);
    auto newerRigid = ScaledIdentity(1.0F, 2.0F, 4.0F, 6.0F);
    newerRigid.values[0] = 0.0F;
    newerRigid.values[1] = -1.0F;
    newerRigid.values[4] = 1.0F;
    newerRigid.values[5] = 0.0F;
    atb::hands_trace::Matrix interpolatedRigid{};
    Require(atb::hands_trace::InterpolateRigid(
                olderRigid, newerRigid, 0.5, interpolatedRigid),
            "ordinary rigid transforms should interpolate");
    Require(std::abs(interpolatedRigid.values[3] - 1.0F) < 1.0e-5F &&
                std::abs(interpolatedRigid.values[7] - 3.0F) < 1.0e-5F &&
                std::abs(interpolatedRigid.values[11] - 5.0F) < 1.0e-5F,
            "rigid interpolation should blend translation");
    Require(std::abs(interpolatedRigid.values[0] - 0.7071067F) < 1.0e-4F &&
                std::abs(interpolatedRigid.values[1] + 0.7071067F) < 1.0e-4F &&
                std::abs(interpolatedRigid.values[4] - 0.7071067F) < 1.0e-4F,
            "rigid interpolation should slerp orientation");
    Require(!atb::hands_trace::InterpolateRigid(
                olderRigid, newerRigid, 1.1, interpolatedRigid),
            "out-of-range rigid interpolation must fail closed");

    std::array<void*, 48> vtable{};
    vtable[0xA8 / sizeof(void*)] = reinterpret_cast<void*>(&GetFakeMatrix);
    vtable[0x100 / sizeof(void*)] = reinterpret_cast<void*>(&GetFakeMeshEntity);
    vtable[0x170 / sizeof(void*)] = reinterpret_cast<void*>(&SetFakeMatrix);
    int meshIdentity = 0;
    FakeEntity entity{vtable.data(), world, &meshIdentity};
    atb::hands_trace::Matrix readBack{};
    Require(atb::hands_trace::ReadEntityMatrix(&entity, readBack) &&
                atb::hands_trace::SameMatrix(readBack, world),
            "mapped +0xA8 virtual matrix getter should be invoked safely");
    Require(!atb::hands_trace::ReadEntityMatrix(nullptr, readBack),
            "null entity matrix read must fail closed");
    Require(atb::hands_trace::GetMeshEntity(&entity) == &meshIdentity,
            "mapped +0x100 virtual mesh getter should return the live owner");
    Require(atb::hands_trace::GetMeshEntity(nullptr) == nullptr,
            "null entity mesh lookup must fail closed");
    Require(atb::hands_trace::WriteEntityMatrix(&entity, hands) &&
                atb::hands_trace::SameMatrix(entity.matrix, hands),
            "mapped +0x170 virtual matrix setter should be invoked safely");
    Require(!atb::hands_trace::WriteEntityMatrix(nullptr, hands),
            "null entity matrix write must fail closed");

    std::cout << "hands trace tests passed\n";
    return EXIT_SUCCESS;
}
