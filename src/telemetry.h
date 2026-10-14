#pragma once

#include "runtime_mapping.h"

#include <string>

namespace atb::telemetry {

struct Options {
    bool telemetryEnabled = false;
    bool motionTraceEnabled = false;
    bool cameraRestoreProbe = false;
    bool cameraInterpolation = false;
    bool handsTraceEnabled = false;
    bool handsRenderCopyProbe = false;
    bool handsPoseRestoreProbe = false;
    bool handsPoseRenderCopyProbe = false;
    bool handsRootInterpolation = false;
    bool handsPoseInterpolation = false;
    bool handsRestoreProbe = false;
    bool renderUnlock = false;
    bool presentationLimiter = false;
    unsigned int targetFps = 0;
    unsigned int reportIntervalSeconds = 5;
    bool worldTraceEnabled = false;
    bool worldRootInterpolation = false;
    unsigned int gameplayRateOverride = 0;
    bool worldPoseInterpolation = false;
    bool mouseTraceEnabled = false;
    bool mousePreview = false;
    bool frameTraceEnabled = false;
    unsigned int framePacingEvenness = 0;
    bool mouseOverlay = false;
    bool particleInterpolation = false;
};

[[nodiscard]] bool Install(const mapping::TelemetryMap& map,
                           const Options& options,
                           std::wstring& error) noexcept;

} // namespace atb::telemetry
