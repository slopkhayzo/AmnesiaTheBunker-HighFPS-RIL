#include "camera_trace.h"

#include "guarded_memory.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace atb::camera_trace {
namespace {

constexpr std::size_t kSceneCurrentListenerOffset = 0x90;
constexpr std::size_t kViewportCameraOffset = 0x18;
constexpr std::size_t kCameraPositionOffset = 0x10;
constexpr std::size_t kCameraPitchOffset = 0x48;
constexpr std::size_t kCameraYawOffset = 0x4C;
constexpr std::size_t kCameraRollOffset = 0x50;

template <typename Value>
bool ReadValue(const std::uintptr_t address, Value& value) noexcept {
    return memory::Read(reinterpret_cast<const void*>(address), value);
}

template <typename Value>
bool ReadAtOffset(const std::uintptr_t base,
                  const std::size_t offset,
                  Value& value) noexcept {
    return base <= std::numeric_limits<std::uintptr_t>::max() - offset &&
           ReadValue(base + offset, value);
}

bool IsFinite(const Sample& sample) noexcept {
    return std::isfinite(sample.position[0]) &&
           std::isfinite(sample.position[1]) &&
           std::isfinite(sample.position[2]) &&
           std::isfinite(sample.pitch) && std::isfinite(sample.yaw) &&
           std::isfinite(sample.roll);
}

} // namespace

SampleStatus ReadCurrentListenerCamera(const std::uintptr_t sceneGlobalPointer,
                                       Sample& sample) noexcept {
    sample = {};
    std::uintptr_t scene = 0;
    if (!ReadValue(sceneGlobalPointer, scene)) {
        return SampleStatus::unreadable;
    }
    if (scene == 0) {
        return SampleStatus::sceneUnavailable;
    }
    if (!ReadAtOffset(scene, kSceneCurrentListenerOffset, sample.listener)) {
        return SampleStatus::unreadable;
    }
    if (sample.listener == 0) {
        return SampleStatus::listenerUnavailable;
    }
    if (!ReadAtOffset(sample.listener, kViewportCameraOffset, sample.camera)) {
        return SampleStatus::unreadable;
    }
    if (sample.camera == 0) {
        return SampleStatus::cameraUnavailable;
    }
    if (sample.camera > std::numeric_limits<std::uintptr_t>::max() -
                            kCameraPositionOffset ||
        !memory::Copy(sample.position,
                      reinterpret_cast<const void*>(sample.camera +
                                                     kCameraPositionOffset),
                      sizeof(sample.position)) ||
        !ReadAtOffset(sample.camera, kCameraPitchOffset, sample.pitch) ||
        !ReadAtOffset(sample.camera, kCameraYawOffset, sample.yaw) ||
        !ReadAtOffset(sample.camera, kCameraRollOffset, sample.roll)) {
        sample = {};
        return SampleStatus::unreadable;
    }
    if (!IsFinite(sample)) {
        return SampleStatus::nonFinite;
    }
    return SampleStatus::ok;
}

bool SameIdentity(const Sample& left, const Sample& right) noexcept {
    return left.listener == right.listener && left.camera == right.camera;
}

bool SameTransform(const Sample& left, const Sample& right) noexcept {
    return std::memcmp(left.position, right.position, sizeof(left.position)) == 0 &&
           std::memcmp(&left.pitch, &right.pitch, sizeof(left.pitch)) == 0 &&
           std::memcmp(&left.yaw, &right.yaw, sizeof(left.yaw)) == 0 &&
           std::memcmp(&left.roll, &right.roll, sizeof(left.roll)) == 0;
}

} // namespace atb::camera_trace
