#include "camera_trace.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Value, std::size_t Size>
void Store(std::array<std::uint8_t, Size>& storage,
           const std::size_t offset,
           const Value& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(value));
}

} // namespace

int main() {
    alignas(void*) std::array<std::uint8_t, 0x100> scene{};
    alignas(void*) std::array<std::uint8_t, 0x40> listener{};
    alignas(void*) std::array<std::uint8_t, 0x80> camera{};
    std::uintptr_t scenePointer = reinterpret_cast<std::uintptr_t>(scene.data());
    const std::uintptr_t listenerPointer =
        reinterpret_cast<std::uintptr_t>(listener.data());
    const std::uintptr_t cameraPointer =
        reinterpret_cast<std::uintptr_t>(camera.data());
    Store(scene, 0x90, listenerPointer);
    Store(listener, 0x18, cameraPointer);
    constexpr float position[3] = {1.0F, -2.0F, 3.5F};
    std::memcpy(camera.data() + 0x10, position, sizeof(position));
    Store(camera, 0x48, 0.25F);
    Store(camera, 0x4C, -0.50F);
    Store(camera, 0x50, 0.75F);

    atb::camera_trace::Sample first{};
    Require(atb::camera_trace::ReadCurrentListenerCamera(
                reinterpret_cast<std::uintptr_t>(&scenePointer), first) ==
                atb::camera_trace::SampleStatus::ok,
            "the frozen scene/listener/camera layout should be readable");
    Require(first.listener == listenerPointer && first.camera == cameraPointer &&
                first.position[0] == 1.0F && first.position[1] == -2.0F &&
                first.position[2] == 3.5F && first.pitch == 0.25F &&
                first.yaw == -0.50F && first.roll == 0.75F,
            "the camera trace should read the expected identity and transform");

    atb::camera_trace::Sample second = first;
    Require(atb::camera_trace::SameIdentity(first, second) &&
                atb::camera_trace::SameTransform(first, second),
            "identical samples should compare equal");
    second.yaw += 0.125F;
    Require(atb::camera_trace::SameIdentity(first, second) &&
                !atb::camera_trace::SameTransform(first, second),
            "transform changes must not be mistaken for identity changes");

    const float nan = std::nanf("");
    Store(camera, 0x48, nan);
    Require(atb::camera_trace::ReadCurrentListenerCamera(
                reinterpret_cast<std::uintptr_t>(&scenePointer), second) ==
                atb::camera_trace::SampleStatus::nonFinite,
            "non-finite camera state should fail closed");
    scenePointer = 0;
    Require(atb::camera_trace::ReadCurrentListenerCamera(
                reinterpret_cast<std::uintptr_t>(&scenePointer), second) ==
                atb::camera_trace::SampleStatus::sceneUnavailable,
            "a missing scene should be reported without dereferencing it");
    scenePointer = UINTPTR_MAX;
    Require(atb::camera_trace::ReadCurrentListenerCamera(
                reinterpret_cast<std::uintptr_t>(&scenePointer), second) ==
                atb::camera_trace::SampleStatus::unreadable,
            "an overflowing scene pointer should fail closed");

    std::cout << "read-only camera trace layout tests passed\n";
    return EXIT_SUCCESS;
}
