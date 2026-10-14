#include "camera_runtime.h"

#include "guarded_memory.h"

#include <algorithm>
#include <cmath>

namespace atb::camera {
namespace {

constexpr std::size_t kSceneCurrentListenerOffset = 0x90;
constexpr std::size_t kViewportCameraOffset = 0x18;
constexpr std::size_t kViewportWorldOffset = 0x20;
constexpr std::size_t kViewportVisibleOffset = 0x29;
constexpr std::size_t kCameraPositionOffset = 0x10;
constexpr std::size_t kCameraPitchOffset = 0x48;
constexpr std::size_t kCameraYawOffset = 0x4C;
constexpr std::size_t kCameraRollOffset = 0x50;
constexpr std::size_t kCameraRotateModeOffset = 0x70;
constexpr std::size_t kCameraViewCachesOffset = 0x78;
constexpr std::size_t kCameraMoveMatrixOffset = 0x138;
constexpr std::size_t kCameraRotationMatrixOffset = 0x1F8;
constexpr std::size_t kCameraPrimaryFrustumOffset = 0x3E0;
constexpr std::size_t kCameraExtendedFrustumPointerOffset = 0x6E8;
constexpr std::size_t kCameraDirtyFlagsOffset = 0x6F1;

template <typename Value>
bool ReadAt(const void* const base,
            const std::size_t offset,
            Value& value) noexcept {
    return base != nullptr &&
           memory::Read(static_cast<const std::uint8_t*>(base) + offset,
                        value);
}

template <typename Value>
bool WriteAt(void* const base,
             const std::size_t offset,
             const Value& value) noexcept {
    return memory::Write(static_cast<std::uint8_t*>(base) + offset, value);
}

template <std::size_t Size>
bool SaveBytes(const void* const base,
               const std::size_t offset,
               std::array<std::uint8_t, Size>& bytes) noexcept {
    return memory::Copy(bytes.data(),
                        static_cast<const std::uint8_t*>(base) + offset,
                        bytes.size());
}

template <std::size_t Size>
bool RestoreBytes(void* const base,
                  const std::size_t offset,
                  const std::array<std::uint8_t, Size>& bytes) noexcept {
    return memory::Copy(static_cast<std::uint8_t*>(base) + offset,
                        bytes.data(), bytes.size());
}

template <std::size_t Size>
bool BytesMatch(const void* const base,
                const std::size_t offset,
                const std::array<std::uint8_t, Size>& bytes) noexcept {
    return memory::Equal(static_cast<const std::uint8_t*>(base) + offset,
                         bytes.data(), bytes.size());
}

} // namespace

TransactionResult Runtime::SelectMainView(void* const scene,
                                          Identity& identity) noexcept {
    identity = {};
    if (scene == nullptr ||
        !ReadAt(scene, kSceneCurrentListenerOffset, identity.listener)) {
        return TransactionResult::invalidLayout;
    }
    if (identity.listener == nullptr) {
        return TransactionResult::noCurrentListener;
    }
    std::uint8_t visible = 0;
    if (!ReadAt(identity.listener, kViewportVisibleOffset, visible) ||
        visible > 1U) {
        return TransactionResult::invalidLayout;
    }
    if (visible == 0U) {
        return TransactionResult::listenerHidden;
    }
    if (!ReadAt(identity.listener, kViewportCameraOffset, identity.camera)) {
        return TransactionResult::invalidLayout;
    }
    if (identity.camera == nullptr) {
        return TransactionResult::listenerMissingCamera;
    }
    if (!ReadAt(identity.listener, kViewportWorldOffset, identity.world)) {
        return TransactionResult::invalidLayout;
    }
    if (identity.world == nullptr) {
        return TransactionResult::listenerMissingWorld;
    }
    std::uint32_t rotateMode = 0;
    if (!ReadAt(identity.camera, kCameraRotateModeOffset, rotateMode)) {
        return TransactionResult::invalidLayout;
    }
    if (rotateMode > 1U) {
        return TransactionResult::unsupportedRotateMode;
    }
    identity.rotateMode = rotateMode;
    return TransactionResult::staged;
}

bool Runtime::SaveState(void* const camera,
                        const std::uint32_t rotateMode,
                        SavedState& state) noexcept {
    state = {};
    if (rotateMode == 1U) {
        if (!SaveBytes(camera, kCameraRotationMatrixOffset,
                       state.rotationMatrix)) {
            return false;
        }
        state.hasRotationMatrix = true;
    }
    if (!ReadPose(camera, state.pose) ||
        !SaveBytes(camera, kCameraViewCachesOffset, state.viewCaches) ||
        !SaveBytes(camera, kCameraMoveMatrixOffset, state.moveMatrix) ||
        !SaveBytes(camera, kCameraPrimaryFrustumOffset,
                   state.primaryFrustum) ||
        !SaveBytes(camera, kCameraDirtyFlagsOffset, state.dirtyFlags) ||
        !ReadAt(camera, kCameraExtendedFrustumPointerOffset,
                state.extendedFrustumPointer)) {
        return false;
    }
    if (state.extendedFrustumPointer != nullptr) {
        if (!SaveBytes(state.extendedFrustumPointer, 0,
                       state.extendedFrustum)) {
            return false;
        }
        state.hasExtendedFrustum = true;
    }
    return true;
}

bool Runtime::ReadPose(void* const camera, Pose& pose) noexcept {
    pose = {};
    return ReadAt(camera, kCameraPositionOffset, pose.position) &&
           ReadAt(camera, kCameraPitchOffset, pose.pitch) &&
           ReadAt(camera, kCameraYawOffset, pose.yaw) &&
           ReadAt(camera, kCameraRollOffset, pose.roll) && IsFinite(pose);
}

bool Runtime::ReadMatrixPose(void* const camera,
                             hands_trace::Matrix& pose) noexcept {
    pose = {};
    Vec3 position{};
    if (!ReadAt(camera, kCameraRotationMatrixOffset, pose) ||
        !ReadAt(camera, kCameraPositionOffset, position) ||
        !hands_trace::IsFiniteAffine(pose) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return false;
    }
    // The engine multiplies this matrix by a separate translation, so its own
    // translation column must be empty before it is reused for the position.
    constexpr float epsilon = 1.0e-4F;
    if (std::abs(pose.values[3]) > epsilon ||
        std::abs(pose.values[7]) > epsilon ||
        std::abs(pose.values[11]) > epsilon) {
        return false;
    }
    pose.values[3] = position.x;
    pose.values[7] = position.y;
    pose.values[11] = position.z;
    return true;
}

bool Runtime::StageMatrixPose(void* const camera,
                              const hands_trace::Matrix& pose,
                              const SavedState& state) noexcept {
    if (!state.hasRotationMatrix || !hands_trace::IsFiniteAffine(pose)) {
        return false;
    }
    const Vec3 position{pose.values[3], pose.values[7], pose.values[11]};
    hands_trace::Matrix rotation = pose;
    rotation.values[3] = 0.0F;
    rotation.values[7] = 0.0F;
    rotation.values[11] = 0.0F;
    std::array<std::uint8_t, 5> dirtyFlags = state.dirtyFlags;
    dirtyFlags[0] = 1U;
    dirtyFlags[2] = 1U;
    dirtyFlags[3] = 1U;
    dirtyFlags[4] = state.hasExtendedFrustum ? 1U : state.dirtyFlags[4];
    if (WriteAt(camera, kCameraPositionOffset, position) &&
        WriteAt(camera, kCameraRotationMatrixOffset, rotation) &&
        RestoreBytes(camera, kCameraDirtyFlagsOffset, dirtyFlags)) {
        return true;
    }
    (void)RestoreState(camera, state);
    return false;
}

void Runtime::ResetMatrixHistory() noexcept {
    matrixOlder_ = {};
    matrixNewer_ = {};
    matrixGeneration_ = 0;
    matrixCount_ = 0;
}

bool Runtime::StagePose(void* const camera,
                        const Pose& pose,
                        const SavedState& state) noexcept {
    if (!IsFinite(pose)) {
        return false;
    }
    // +0x6F1 view, +0x6F3 move, +0x6F4 primary frustum,
    // +0x6F5 extended frustum. Projection (+0x6F2) is independent.
    std::array<std::uint8_t, 5> dirtyFlags = state.dirtyFlags;
    dirtyFlags[0] = 1U;
    dirtyFlags[2] = 1U;
    dirtyFlags[3] = 1U;
    // Do not force the lazy extended-frustum allocation. If it already
    // exists, exercise and restore it; otherwise retain the authoritative bit.
    dirtyFlags[4] = state.hasExtendedFrustum ? 1U : state.dirtyFlags[4];
    if (WriteAt(camera, kCameraPositionOffset, pose.position) &&
        WriteAt(camera, kCameraPitchOffset, pose.pitch) &&
        WriteAt(camera, kCameraYawOffset, pose.yaw) &&
        WriteAt(camera, kCameraRollOffset, pose.roll) &&
        RestoreBytes(camera, kCameraDirtyFlagsOffset, dirtyFlags)) {
        return true;
    }
    // A partially staged pose must never reach the renderer or the next
    // update: put back whatever was written before reporting the failure.
    (void)RestoreState(camera, state);
    return false;
}

bool Runtime::RestoreState(void* const camera,
                           const SavedState& state) noexcept {
    void* currentExtended = nullptr;
    if (!ReadAt(camera, kCameraExtendedFrustumPointerOffset, currentExtended) ||
        currentExtended != state.extendedFrustumPointer ||
        !WriteAt(camera, kCameraPositionOffset, state.pose.position) ||
        !WriteAt(camera, kCameraPitchOffset, state.pose.pitch) ||
        !WriteAt(camera, kCameraYawOffset, state.pose.yaw) ||
        !WriteAt(camera, kCameraRollOffset, state.pose.roll)) {
        return false;
    }
    if (state.hasRotationMatrix &&
        !RestoreBytes(camera, kCameraRotationMatrixOffset,
                      state.rotationMatrix)) {
        return false;
    }
    if (!RestoreBytes(camera, kCameraViewCachesOffset, state.viewCaches) ||
        !RestoreBytes(camera, kCameraMoveMatrixOffset, state.moveMatrix) ||
        !RestoreBytes(camera, kCameraPrimaryFrustumOffset,
                      state.primaryFrustum) ||
        (state.hasExtendedFrustum &&
         !RestoreBytes(state.extendedFrustumPointer, 0,
                       state.extendedFrustum)) ||
        !RestoreBytes(camera, kCameraDirtyFlagsOffset, state.dirtyFlags)) {
        return false;
    }
    return true;
}

bool Runtime::StateMatches(void* const camera,
                           const SavedState& state) noexcept {
    Pose pose{};
    void* extended = nullptr;
    if (!ReadAt(camera, kCameraPositionOffset, pose.position) ||
        !ReadAt(camera, kCameraPitchOffset, pose.pitch) ||
        !ReadAt(camera, kCameraYawOffset, pose.yaw) ||
        !ReadAt(camera, kCameraRollOffset, pose.roll) ||
        !NearlyEqual(pose, state.pose, 0.0F) ||
        !ReadAt(camera, kCameraExtendedFrustumPointerOffset, extended) ||
        extended != state.extendedFrustumPointer ||
        !BytesMatch(camera, kCameraViewCachesOffset, state.viewCaches) ||
        !BytesMatch(camera, kCameraMoveMatrixOffset, state.moveMatrix) ||
        !BytesMatch(camera, kCameraPrimaryFrustumOffset,
                    state.primaryFrustum) ||
        !BytesMatch(camera, kCameraDirtyFlagsOffset, state.dirtyFlags)) {
        return false;
    }
    if (state.hasRotationMatrix &&
        !BytesMatch(camera, kCameraRotationMatrixOffset,
                    state.rotationMatrix)) {
        return false;
    }
    return !state.hasExtendedFrustum ||
           BytesMatch(state.extendedFrustumPointer, 0,
                      state.extendedFrustum);
}

TransactionResult Runtime::BeginIdentityProbe(void* const scene) noexcept {
    if (active_) {
        return TransactionResult::alreadyActive;
    }
    Identity selected{};
    const TransactionResult selection = SelectMainView(scene, selected);
    if (selection != TransactionResult::staged) {
        return selection;
    }
    if (selected.rotateMode != 0U) {
        return TransactionResult::unsupportedRotateMode;
    }
    SavedState state{};
    if (!SaveState(selected.camera, selected.rotateMode, state)) {
        return TransactionResult::invalidCamera;
    }
    if (!StagePose(selected.camera, state.pose, state)) {
        return TransactionResult::invalidCamera;
    }
    identity_ = selected;
    saved_ = state;
    active_ = true;
    return TransactionResult::staged;
}

void Runtime::Reset() noexcept {
    history_.Reset();
    ResetMatrixHistory();
    identity_ = {};
    saved_ = {};
    hasIdentity_ = false;
    active_ = false;
}

TransactionResult Runtime::Capture(void* const scene,
                                   const std::uint64_t serial,
                                   const std::uint64_t clockGeneration) noexcept {
    if (active_) {
        return TransactionResult::alreadyActive;
    }
    Identity selected{};
    const TransactionResult selection = SelectMainView(scene, selected);
    if (selection != TransactionResult::staged) {
        history_.Reset();
        ResetMatrixHistory();
        hasIdentity_ = false;
        return selection;
    }
    const bool identityChanged =
        !hasIdentity_ || selected.listener != identity_.listener ||
        selected.world != identity_.world ||
        selected.camera != identity_.camera ||
        selected.rotateMode != identity_.rotateMode;
    if (selected.rotateMode == 1U) {
        hands_trace::Matrix matrixPose{};
        if (!ReadMatrixPose(selected.camera, matrixPose)) {
            history_.Reset();
            ResetMatrixHistory();
            hasIdentity_ = false;
            return TransactionResult::invalidCamera;
        }
        history_.Reset();
        const bool adjacent = !identityChanged && matrixCount_ != 0 &&
                              matrixGeneration_ == clockGeneration &&
                              serial == matrixNewer_.serial + 1;
        const bool sameTick = !identityChanged && matrixCount_ != 0 &&
                              matrixGeneration_ == clockGeneration &&
                              serial == matrixNewer_.serial;
        identity_ = selected;
        hasIdentity_ = true;
        if (sameTick) {
            if (!hands_trace::SameMatrix(matrixNewer_.pose, matrixPose)) {
                matrixNewer_.pose = matrixPose;
                matrixCount_ = 1;
                return TransactionResult::discontinuityReset;
            }
            return TransactionResult::captured;
        }
        if (adjacent) {
            matrixOlder_ = matrixNewer_;
            matrixNewer_ = {serial, matrixPose};
            matrixCount_ = 2;
            return TransactionResult::captured;
        }
        matrixNewer_ = {serial, matrixPose};
        matrixGeneration_ = clockGeneration;
        matrixCount_ = 1;
        return identityChanged ? TransactionResult::identityChanged
                               : TransactionResult::discontinuityReset;
    }
    ResetMatrixHistory();
    Pose pose{};
    if (!ReadPose(selected.camera, pose)) {
        history_.Reset();
        hasIdentity_ = false;
        return TransactionResult::invalidCamera;
    }
    if (identityChanged) {
        history_.Reset();
        identity_ = selected;
        hasIdentity_ = true;
        (void)history_.Publish(serial, clockGeneration, pose);
        return TransactionResult::identityChanged;
    }
    const PublishResult published =
        history_.Publish(serial, clockGeneration, pose);
    switch (published.kind) {
    case PublishKind::appended:
    case PublishKind::unchanged:
        return TransactionResult::captured;
    case PublishKind::resetGeneration:
    case PublishKind::resetNonMonotonic:
    case PublishKind::resetSameTickMutation:
    case PublishKind::resetDiscontinuity:
        return TransactionResult::discontinuityReset;
    case PublishKind::invalid:
        return TransactionResult::invalidCamera;
    }
    return TransactionResult::invalidCamera;
}

TransactionResult Runtime::BeginInterpolation(
    void* const scene,
    const clock::Sample& clockSample,
    const LookLead* const lead,
    StagedLook* const look) noexcept {
    if (look != nullptr) {
        *look = {};
    }
    if (active_) {
        return TransactionResult::alreadyActive;
    }
    Identity selected{};
    const TransactionResult selection = SelectMainView(scene, selected);
    if (selection != TransactionResult::staged) {
        return selection;
    }
    if (!hasIdentity_ || selected.listener != identity_.listener ||
        selected.world != identity_.world ||
        selected.camera != identity_.camera ||
        selected.rotateMode != identity_.rotateMode) {
        history_.Reset();
        ResetMatrixHistory();
        identity_ = selected;
        hasIdentity_ = true;
        return TransactionResult::identityChanged;
    }
    if (selected.rotateMode == 1U) {
        if (clockSample.kind != clock::SampleKind::interpolated ||
            clockSample.generation != matrixGeneration_) {
            return TransactionResult::clockUnavailable;
        }
        if (matrixCount_ != 2 ||
            matrixOlder_.serial != clockSample.olderSerial ||
            matrixNewer_.serial != clockSample.newerSerial) {
            return TransactionResult::snapshotsMissing;
        }
        // Rejects non-rigid matrices and steps beyond 5 m or 120 degrees,
        // which is how a scripted camera cut shows up.
        hands_trace::Matrix interpolated{};
        if (!hands_trace::InterpolateRigid(matrixOlder_.pose,
                                           matrixNewer_.pose,
                                           clockSample.alpha, interpolated)) {
            return TransactionResult::snapshotsMissing;
        }
        SavedState state{};
        if (!SaveState(selected.camera, selected.rotateMode, state)) {
            return TransactionResult::invalidCamera;
        }
        if (!StageMatrixPose(selected.camera, interpolated, state)) {
            return TransactionResult::applyFailed;
        }
        saved_ = state;
        identity_ = selected;
        active_ = true;
        return TransactionResult::interpolated;
    }
    const SampleResult sampled = history_.Evaluate(clockSample);
    if (sampled.kind == SampleKind::clockUnavailable ||
        sampled.kind == SampleKind::generationMismatch) {
        return TransactionResult::clockUnavailable;
    }
    if (sampled.kind == SampleKind::snapshotsMissing) {
        return TransactionResult::snapshotsMissing;
    }
    if (sampled.kind != SampleKind::applied) {
        return TransactionResult::invalidCamera;
    }
    Pose pose = sampled.pose;
    float leadYaw = 0.0F;
    float leadPitch = 0.0F;
    if (lead != nullptr && std::isfinite(lead->yaw) &&
        std::isfinite(lead->pitch)) {
        // Never pull the view inside a limit the game itself has exceeded.
        const float low = std::min(lead->pitchLow, pose.pitch);
        const float high = std::max(lead->pitchHigh, pose.pitch);
        const float pitch = std::clamp(pose.pitch + lead->pitch, low, high);
        leadYaw = lead->yaw;
        leadPitch = pitch - pose.pitch;
        pose.yaw += leadYaw;
        pose.pitch = pitch;
    }
    SavedState state{};
    if (!SaveState(selected.camera, selected.rotateMode, state)) {
        return TransactionResult::invalidCamera;
    }
    if (!StagePose(selected.camera, pose, state)) {
        return TransactionResult::applyFailed;
    }
    saved_ = state;
    identity_ = selected;
    active_ = true;
    if (look != nullptr) {
        look->staged = true;
        look->viewYaw = pose.yaw;
        look->viewPitch = pose.pitch;
    }
    if (look != nullptr && lead != nullptr) {
        look->applied = true;
        look->yaw = leadYaw;
        look->pitch = leadPitch;
        look->position = pose.position;
    }
    return TransactionResult::interpolated;
}

TransactionResult Runtime::Restore() noexcept {
    if (!active_) {
        return TransactionResult::restored;
    }
    void* camera = nullptr;
    void* world = nullptr;
    if (!ReadAt(identity_.listener, kViewportCameraOffset, camera) ||
        !ReadAt(identity_.listener, kViewportWorldOffset, world) ||
        camera != identity_.camera || world != identity_.world) {
        active_ = false;
        return TransactionResult::identityChanged;
    }
    const bool restored = RestoreState(identity_.camera, saved_) &&
                          StateMatches(identity_.camera, saved_);
    active_ = false;
    return restored ? TransactionResult::restored
                    : TransactionResult::restoreFailed;
}

const wchar_t* TransactionResultName(const TransactionResult result) noexcept {
    switch (result) {
    case TransactionResult::captured: return L"captured";
    case TransactionResult::interpolated: return L"interpolated";
    case TransactionResult::staged: return L"staged";
    case TransactionResult::restored: return L"restored";
    case TransactionResult::noCurrentListener: return L"no-current-listener";
    case TransactionResult::listenerHidden: return L"listener-hidden";
    case TransactionResult::listenerMissingCamera: return L"missing-camera";
    case TransactionResult::listenerMissingWorld: return L"missing-world";
    case TransactionResult::invalidLayout: return L"invalid-layout";
    case TransactionResult::unsupportedRotateMode: return L"rotate-mode";
    case TransactionResult::invalidCamera: return L"invalid-camera";
    case TransactionResult::alreadyActive: return L"already-active";
    case TransactionResult::identityChanged: return L"identity-changed";
    case TransactionResult::clockUnavailable: return L"clock-unavailable";
    case TransactionResult::snapshotsMissing: return L"snapshots-missing";
    case TransactionResult::discontinuityReset: return L"discontinuity-reset";
    case TransactionResult::applyFailed: return L"apply-failed";
    case TransactionResult::restoreFailed: return L"restore-failed";
    }
    return L"unknown";
}

} // namespace atb::camera
