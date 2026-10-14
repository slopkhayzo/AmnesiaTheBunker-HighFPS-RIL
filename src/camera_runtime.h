#pragma once

#include "camera_interpolation.h"
#include "hands_trace.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace atb::camera {

// Extra look rotation the presented camera should lead the interpolated one
// by, in the camera's own yaw and pitch fields.
struct LookLead {
    float yaw = 0.0F;
    float pitch = 0.0F;
    float pitchLow = -1.6F;
    float pitchHigh = 1.6F;
};

// What was actually staged, so everything attached to the view can follow.
struct StagedLook {
    bool applied = false;
    float yaw = 0.0F;
    float pitch = 0.0F;
    Vec3 position{};
    // The Euler angles staged for the render, with or without a lead.
    bool staged = false;
    float viewYaw = 0.0F;
    float viewPitch = 0.0F;
};

enum class TransactionResult : std::uint32_t {
    captured,
    interpolated,
    staged,
    restored,
    noCurrentListener,
    listenerHidden,
    listenerMissingCamera,
    listenerMissingWorld,
    invalidLayout,
    unsupportedRotateMode,
    invalidCamera,
    alreadyActive,
    identityChanged,
    clockUnavailable,
    snapshotsMissing,
    discontinuityReset,
    applyFailed,
    restoreFailed,
};

class Runtime final {
public:
    void Reset() noexcept;
    [[nodiscard]] TransactionResult Capture(void* scene,
                                            std::uint64_t serial,
                                            std::uint64_t clockGeneration) noexcept;
    // `lead` applies only to Euler-mode cameras; `look` reports what was
    // staged. Both may be null.
    [[nodiscard]] TransactionResult BeginInterpolation(
        void* scene,
        const clock::Sample& clockSample,
        const LookLead* lead = nullptr,
        StagedLook* look = nullptr) noexcept;
    // Development safety gate: stage the current authoritative pose while
    // forcing only its proven derived caches dirty. No interpolation occurs.
    [[nodiscard]] TransactionResult BeginIdentityProbe(void* scene) noexcept;
    [[nodiscard]] TransactionResult Restore() noexcept;
    [[nodiscard]] bool IsActive() const noexcept { return active_; }

private:
    struct Identity {
        void* listener = nullptr;
        void* world = nullptr;
        void* camera = nullptr;
        // 0: Euler angles drive the view. 1: the view rotation is the
        // matrix at camera+0x1F8 (scripted cameras and cutscenes).
        std::uint32_t rotateMode = 0;
    };

    // Two-tick history for matrix-mode cameras. The rotation part holds the
    // engine's view-rotation matrix and the translation part holds the camera
    // position, so one rigid interpolation covers both.
    struct MatrixSnapshot {
        std::uint64_t serial = 0;
        hands_trace::Matrix pose{};
    };

    struct SavedState {
        Pose pose{};
        std::array<std::uint8_t, 0x80> viewCaches{};
        std::array<std::uint8_t, 0x40> moveMatrix{};
        std::array<std::uint8_t, 0x308> primaryFrustum{};
        std::array<std::uint8_t, 0x308> extendedFrustum{};
        std::array<std::uint8_t, 5> dirtyFlags{};
        std::array<std::uint8_t, 0x40> rotationMatrix{};
        bool hasRotationMatrix = false;
        void* extendedFrustumPointer = nullptr;
        bool hasExtendedFrustum = false;
    };

    [[nodiscard]] static TransactionResult SelectMainView(
        void* scene, Identity& identity) noexcept;
    [[nodiscard]] static bool SaveState(void* camera,
                                        std::uint32_t rotateMode,
                                        SavedState& state) noexcept;
    [[nodiscard]] static bool ReadMatrixPose(void* camera,
                                             hands_trace::Matrix& pose) noexcept;
    [[nodiscard]] static bool StageMatrixPose(void* camera,
                                              const hands_trace::Matrix& pose,
                                              const SavedState& state) noexcept;
    void ResetMatrixHistory() noexcept;
    [[nodiscard]] static bool ReadPose(void* camera, Pose& pose) noexcept;
    [[nodiscard]] static bool StagePose(void* camera,
                                        const Pose& pose,
                                        const SavedState& state) noexcept;
    [[nodiscard]] static bool RestoreState(void* camera,
                                           const SavedState& state) noexcept;
    [[nodiscard]] static bool StateMatches(void* camera,
                                           const SavedState& state) noexcept;

    Identity identity_{};
    SavedState saved_{};
    History history_{};
    MatrixSnapshot matrixOlder_{};
    MatrixSnapshot matrixNewer_{};
    std::uint64_t matrixGeneration_ = 0;
    std::size_t matrixCount_ = 0;
    bool hasIdentity_ = false;
    bool active_ = false;
};

[[nodiscard]] const wchar_t* TransactionResultName(
    TransactionResult result) noexcept;

} // namespace atb::camera
