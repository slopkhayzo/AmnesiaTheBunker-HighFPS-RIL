#pragma once

#include <string>

namespace atb::config {

struct Features {
    bool renderUnlock = false;
    bool presentationLimiter = false;
    bool cameraInterpolation = false;
    bool handsRootInterpolation = false;
    bool worldRootInterpolation = false;
    bool handsPoseInterpolation = false;
    bool worldPoseInterpolation = false;
    bool ragdollPoseInterpolation = false;
    bool particleInterpolation = false;
    bool imageTrailTiming = false;
    bool mouseOverlay = false;
    bool mousePreview = false;
};

struct Settings {
    bool enabled = true;
    bool diagnostics = true;
    bool telemetryEnabled = false;
    bool motionTraceEnabled = false;
    bool cameraRestoreProbe = false;
    bool handsTraceEnabled = false;
    bool handsRenderCopyProbe = false;
    bool handsPoseRestoreProbe = false;
    bool handsPoseRenderCopyProbe = false;
    bool handsRestoreProbe = false;
    bool worldTraceEnabled = false;
    bool mouseTraceEnabled = false;
    bool frameTraceEnabled = false;
    unsigned int telemetryReportIntervalSeconds = 5;
    Features features{};
    unsigned int targetFps = 0;
    unsigned int gameplayRateOverride = 0;
    // 0..100; only shapes what the presentation limiter does.
    unsigned int framePacingEvenness = 0;
};

struct LoadResult {
    bool ok = false;
    Settings settings{};
    std::wstring error;
};

[[nodiscard]] LoadResult Load(const std::wstring& path);
// Switches on the internal read-only layers the enabled features are built
// on, so a configuration only has to name the features themselves.
void ApplyImpliedSettings(Settings& settings) noexcept;
[[nodiscard]] bool AnyBehavioralFeatureEnabled(const Features& features) noexcept;
[[nodiscard]] bool AnyUnsupportedBehavioralFeatureEnabled(
    const Features& features) noexcept;
[[nodiscard]] bool AnyRuntimeMappingRequested(const Settings& settings) noexcept;

} // namespace atb::config
