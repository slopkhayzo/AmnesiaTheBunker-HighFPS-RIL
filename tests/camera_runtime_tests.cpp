#include "camera_runtime.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <std::size_t Size, typename Value>
void Store(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const Value& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct Fixture {
    alignas(void*) std::array<std::uint8_t, 0x100> scene{};
    alignas(void*) std::array<std::uint8_t, 0x100> viewport{};
    alignas(void*) std::array<std::uint8_t, 0x740> camera{};
    alignas(void*) std::array<std::uint8_t, 0x40> world{};
    alignas(void*) std::array<std::uint8_t, 0x308> extended{};

    Fixture() {
        void* viewportPointer = viewport.data();
        void* cameraPointer = camera.data();
        void* worldPointer = world.data();
        void* extendedPointer = extended.data();
        Store(scene, 0x90, viewportPointer);
        Store(viewport, 0x18, cameraPointer);
        Store(viewport, 0x20, worldPointer);
        viewport[0x29] = 1;
        Store(camera, 0x6E8, extendedPointer);
        const atb::camera::Vec3 position{1.0F, 2.0F, 3.0F};
        Store(camera, 0x10, position);
        Store(camera, 0x48, 0.1F);
        Store(camera, 0x4C, 0.2F);
        Store(camera, 0x50, 0.3F);
        const std::uint32_t rotateMode = 0;
        Store(camera, 0x70, rotateMode);
        for (std::size_t i = 0x78; i < 0xF8; ++i) {
            camera[i] = static_cast<std::uint8_t>(i);
        }
        for (std::size_t i = 0x138; i < 0x178; ++i) {
            camera[i] = static_cast<std::uint8_t>(i * 3U);
        }
        for (std::size_t i = 0; i < 0x308; ++i) {
            camera[0x3E0 + i] = static_cast<std::uint8_t>(i * 5U);
            extended[i] = static_cast<std::uint8_t>(i * 7U);
        }
        camera[0x6F1] = 0;
        camera[0x6F2] = 1;
        camera[0x6F3] = 0;
        camera[0x6F4] = 0;
        camera[0x6F5] = 0;
    }
};

void TestIdentityTransactionRestoresProvenState() {
    Fixture fixture;
    const auto originalCamera = fixture.camera;
    const auto originalExtended = fixture.extended;
    atb::camera::Runtime runtime;
    Require(runtime.BeginIdentityProbe(fixture.scene.data()) ==
                atb::camera::TransactionResult::staged && runtime.IsActive(),
            "valid current-listener camera was not staged");
    Require(fixture.camera[0x6F1] == 1 && fixture.camera[0x6F2] == 1 &&
                fixture.camera[0x6F3] == 1 && fixture.camera[0x6F4] == 1 &&
                fixture.camera[0x6F5] == 1,
            "the probe dirtied anything other than the four derived domains");

    std::memset(fixture.camera.data() + 0x78, 0xCC, 0x80);
    std::memset(fixture.camera.data() + 0x138, 0xDD, 0x40);
    std::memset(fixture.camera.data() + 0x3E0, 0xEE, 0x308);
    std::memset(fixture.extended.data(), 0xFF, 0x308);
    fixture.camera[0x6F1] = 0;
    fixture.camera[0x6F3] = 0;
    fixture.camera[0x6F4] = 0;
    fixture.camera[0x6F5] = 0;

    Require(runtime.Restore() == atb::camera::TransactionResult::restored &&
                !runtime.IsActive(),
            "camera transaction did not restore cleanly");
    Require(std::memcmp(fixture.camera.data(), originalCamera.data(),
                        fixture.camera.size()) == 0,
            "camera bytes differed after the synthetic render transaction");
    Require(fixture.extended == originalExtended,
            "extended frustum bytes differed after restoration");
}

void TestSelectionGuards() {
    Fixture fixture;
    atb::camera::Runtime runtime;
    fixture.viewport[0x29] = 0;
    Require(runtime.BeginIdentityProbe(fixture.scene.data()) ==
                atb::camera::TransactionResult::listenerHidden,
            "hidden listener was accepted");
    fixture.viewport[0x29] = 1;
    const std::uint32_t matrixRotateMode = 1;
    Store(fixture.camera, 0x70, matrixRotateMode);
    Require(runtime.BeginIdentityProbe(fixture.scene.data()) ==
                atb::camera::TransactionResult::unsupportedRotateMode,
            "the Euler-only probe accepted a matrix-mode camera");
    const std::uint32_t unknownRotateMode = 2;
    Store(fixture.camera, 0x70, unknownRotateMode);
    Require(runtime.Capture(fixture.scene.data(), 1, 1) ==
                atb::camera::TransactionResult::unsupportedRotateMode,
            "an unknown rotate mode was accepted");
}

std::array<float, 16> RotationAboutY(const float angle) {
    std::array<float, 16> matrix{};
    matrix[0] = std::cos(angle);
    matrix[2] = std::sin(angle);
    matrix[5] = 1.0F;
    matrix[8] = -std::sin(angle);
    matrix[10] = std::cos(angle);
    matrix[15] = 1.0F;
    return matrix;
}

void TestMatrixModeInterpolatesAndRestores() {
    Fixture fixture;
    const std::uint32_t matrixRotateMode = 1;
    Store(fixture.camera, 0x70, matrixRotateMode);
    const auto olderRotation = RotationAboutY(0.0F);
    const auto newerRotation = RotationAboutY(0.4F);
    Store(fixture.camera, 0x1F8, olderRotation);
    atb::camera::Runtime runtime;
    Require(runtime.Capture(fixture.scene.data(), 20, 5) ==
                atb::camera::TransactionResult::identityChanged,
            "first matrix-mode capture did not establish identity");
    const atb::camera::Vec3 newerPosition{3.0F, 4.0F, 5.0F};
    Store(fixture.camera, 0x10, newerPosition);
    Store(fixture.camera, 0x1F8, newerRotation);
    Require(runtime.Capture(fixture.scene.data(), 21, 5) ==
                atb::camera::TransactionResult::captured,
            "second matrix-mode pose was not captured");
    const auto authoritative = fixture.camera;

    atb::clock::Sample sample{};
    sample.kind = atb::clock::SampleKind::interpolated;
    sample.generation = 5;
    sample.olderSerial = 20;
    sample.newerSerial = 21;
    sample.alpha = 0.5;
    Require(runtime.BeginInterpolation(fixture.scene.data(), sample) ==
                atb::camera::TransactionResult::interpolated,
            "matrix-mode pair was not staged for interpolation");
    atb::camera::Vec3 stagedPosition{};
    std::array<float, 16> stagedRotation{};
    std::memcpy(&stagedPosition, fixture.camera.data() + 0x10,
                sizeof(stagedPosition));
    std::memcpy(stagedRotation.data(), fixture.camera.data() + 0x1F8,
                sizeof(stagedRotation));
    const auto expected = RotationAboutY(0.2F);
    Require(std::abs(stagedPosition.x - 2.0F) < 1.0e-5F &&
                std::abs(stagedPosition.y - 3.0F) < 1.0e-5F &&
                std::abs(stagedPosition.z - 4.0F) < 1.0e-5F,
            "matrix-mode position was not the midpoint");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        Require(std::abs(stagedRotation[index] - expected[index]) < 1.0e-4F,
                "matrix-mode rotation was not the midpoint rotation");
    }
    Require(fixture.camera[0x6F1] == 1 && fixture.camera[0x6F3] == 1 &&
                fixture.camera[0x6F4] == 1,
            "matrix-mode staging did not dirty the derived caches");
    Require(runtime.Restore() == atb::camera::TransactionResult::restored,
            "matrix-mode transaction did not restore");
    Require(fixture.camera == authoritative,
            "matrix-mode camera bytes differed after restoration");

    // A scripted cut is far beyond one tick of motion and must not blend.
    const auto cutRotation = RotationAboutY(3.0F);
    Store(fixture.camera, 0x1F8, cutRotation);
    Require(runtime.Capture(fixture.scene.data(), 22, 5) ==
                atb::camera::TransactionResult::captured,
            "post-cut pose was not captured");
    sample.olderSerial = 21;
    sample.newerSerial = 22;
    Require(runtime.BeginInterpolation(fixture.scene.data(), sample) ==
                atb::camera::TransactionResult::snapshotsMissing &&
                !runtime.IsActive(),
            "a camera cut was blended");

    // Switching back to Euler mode starts a fresh history.
    const std::uint32_t eulerRotateMode = 0;
    Store(fixture.camera, 0x70, eulerRotateMode);
    Require(runtime.Capture(fixture.scene.data(), 23, 5) ==
                atb::camera::TransactionResult::identityChanged,
            "a rotate-mode change did not reset the camera history");
}

void TestIdentityChangeFailsClosed() {
    Fixture fixture;
    atb::camera::Runtime runtime;
    Require(runtime.BeginIdentityProbe(fixture.scene.data()) ==
                atb::camera::TransactionResult::staged,
            "identity-change fixture was not staged");
    void* noCamera = nullptr;
    Store(fixture.viewport, 0x18, noCamera);
    Require(runtime.Restore() ==
                atb::camera::TransactionResult::identityChanged,
            "camera replacement during render did not fail closed");
}

void TestInterpolationStagesAndRestoresAuthoritativePose() {
    Fixture fixture;
    atb::camera::Runtime runtime;
    Require(runtime.Capture(fixture.scene.data(), 10, 3) ==
                atb::camera::TransactionResult::identityChanged,
            "first capture did not establish camera identity");
    const atb::camera::Vec3 newerPosition{3.0F, 4.0F, 5.0F};
    Store(fixture.camera, 0x10, newerPosition);
    Store(fixture.camera, 0x48, 0.3F);
    Store(fixture.camera, 0x4C, 0.4F);
    Store(fixture.camera, 0x50, 0.5F);
    Require(runtime.Capture(fixture.scene.data(), 11, 3) ==
                atb::camera::TransactionResult::captured,
            "second authoritative pose was not captured");

    atb::clock::Sample sample{};
    sample.kind = atb::clock::SampleKind::interpolated;
    sample.generation = 3;
    sample.olderSerial = 10;
    sample.newerSerial = 11;
    sample.alpha = 0.5;
    Require(runtime.BeginInterpolation(fixture.scene.data(), sample) ==
                atb::camera::TransactionResult::interpolated,
            "valid camera pair was not staged for interpolation");
    atb::camera::Vec3 stagedPosition{};
    std::memcpy(&stagedPosition, fixture.camera.data() + 0x10,
                sizeof(stagedPosition));
    Require(std::abs(stagedPosition.x - 2.0F) < 1.0e-5F &&
                std::abs(stagedPosition.y - 3.0F) < 1.0e-5F &&
                std::abs(stagedPosition.z - 4.0F) < 1.0e-5F,
            "render-time position was not the expected midpoint");
    Require(runtime.Restore() == atb::camera::TransactionResult::restored,
            "interpolated transaction did not restore");

    // A look lead adds to the interpolated angles, respects the pitch limit,
    // and still restores the authoritative pose byte-for-byte.
    const auto beforeLead = fixture.camera;
    Require(runtime.BeginInterpolation(fixture.scene.data(), sample) ==
                atb::camera::TransactionResult::interpolated,
            "the reference pair was not staged");
    float basePitch = 0.0F;
    float baseYaw = 0.0F;
    std::memcpy(&basePitch, fixture.camera.data() + 0x48, sizeof(basePitch));
    std::memcpy(&baseYaw, fixture.camera.data() + 0x4C, sizeof(baseYaw));
    Require(runtime.Restore() == atb::camera::TransactionResult::restored,
            "the reference transaction did not restore");
    atb::camera::LookLead lead{};
    lead.yaw = 0.05F;
    lead.pitch = 0.5F;
    lead.pitchLow = -1.0F;
    lead.pitchHigh = basePitch + 0.15F;
    atb::camera::StagedLook look{};
    Require(runtime.BeginInterpolation(fixture.scene.data(), sample, &lead,
                                       &look) ==
                atb::camera::TransactionResult::interpolated,
            "a led camera pair was not staged");
    float stagedPitch = 0.0F;
    float stagedYaw = 0.0F;
    std::memcpy(&stagedPitch, fixture.camera.data() + 0x48,
                sizeof(stagedPitch));
    std::memcpy(&stagedYaw, fixture.camera.data() + 0x4C, sizeof(stagedYaw));
    Require(std::abs(stagedYaw - (baseYaw + 0.05F)) < 1.0e-5F &&
                std::abs(stagedPitch - (basePitch + 0.15F)) < 1.0e-5F,
            "the look lead was not added and clamped");
    Require(look.applied && std::abs(look.yaw - 0.05F) < 1.0e-6F &&
                std::abs(look.pitch - 0.15F) < 1.0e-5F &&
                std::abs(look.position.x - 2.0F) < 1.0e-5F,
            "the staged look was not reported");
    Require(runtime.Restore() == atb::camera::TransactionResult::restored &&
                fixture.camera == beforeLead,
            "a led transaction did not restore the authoritative camera");
    atb::camera::Vec3 restoredPosition{};
    std::memcpy(&restoredPosition, fixture.camera.data() + 0x10,
                sizeof(restoredPosition));
    Require(std::memcmp(&restoredPosition, &newerPosition,
                        sizeof(newerPosition)) == 0,
            "authoritative position was not restored after interpolation");
}

} // namespace

int main() {
    TestIdentityTransactionRestoresProvenState();
    TestSelectionGuards();
    TestMatrixModeInterpolatesAndRestores();
    TestIdentityChangeFailsClosed();
    TestInterpolationStagesAndRestoresAuthoritativePose();
    std::cout << "camera runtime transaction tests passed\n";
    return EXIT_SUCCESS;
}
