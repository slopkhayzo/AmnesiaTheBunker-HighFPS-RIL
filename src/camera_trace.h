#pragma once

#include <cstdint>

namespace atb::camera_trace {

enum class SampleStatus : std::uint8_t {
    ok,
    sceneUnavailable,
    listenerUnavailable,
    cameraUnavailable,
    unreadable,
    nonFinite,
};

struct Sample {
    std::uintptr_t listener = 0;
    std::uintptr_t camera = 0;
    float position[3]{};
    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
};

[[nodiscard]] SampleStatus ReadCurrentListenerCamera(
    std::uintptr_t sceneGlobalPointer,
    Sample& sample) noexcept;

[[nodiscard]] bool SameIdentity(const Sample& left,
                                const Sample& right) noexcept;
[[nodiscard]] bool SameTransform(const Sample& left,
                                 const Sample& right) noexcept;

} // namespace atb::camera_trace
