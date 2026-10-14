#include "config.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

// The INI shipped in the release archives: every validated feature on, no
// diagnostics, and nothing the plugin would reject.
int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) {
        std::cerr << "usage: release_config_tests <release.ini>\n";
        return EXIT_FAILURE;
    }
    atb::config::LoadResult loaded = atb::config::Load(arguments[1]);
    Require(loaded.ok, "release INI should parse");
    atb::config::Settings& settings = loaded.settings;
    const atb::config::Features& features = settings.features;

    Require(settings.enabled && !settings.diagnostics,
            "release should be enabled without diagnostics");
    Require(!settings.telemetryEnabled && !settings.motionTraceEnabled &&
                !settings.handsTraceEnabled && !settings.worldTraceEnabled &&
                !settings.mouseTraceEnabled && !settings.frameTraceEnabled,
            "release INI should not name internal layers or traces");
    Require(features.renderUnlock && features.presentationLimiter &&
                features.cameraInterpolation &&
                features.handsRootInterpolation &&
                features.handsPoseInterpolation &&
                features.worldRootInterpolation &&
                features.worldPoseInterpolation &&
                features.particleInterpolation && features.mousePreview &&
                features.mouseOverlay,
            "release should enable every validated feature");
    Require(!atb::config::AnyUnsupportedBehavioralFeatureEnabled(features),
            "release must not request an unimplemented feature");
    Require(settings.targetFps == 0 && settings.framePacingEvenness == 0 &&
                settings.gameplayRateOverride == 60,
            "release timing defaults: display rate, no frame pacing, 60 Hz "
            "simulation");

    atb::config::ApplyImpliedSettings(settings);
    Require(settings.telemetryEnabled && settings.motionTraceEnabled &&
                settings.handsTraceEnabled && settings.worldTraceEnabled &&
                settings.mouseTraceEnabled,
            "the features should switch on the layers they are built on");
    Require(!settings.frameTraceEnabled && !settings.diagnostics &&
                !settings.cameraRestoreProbe && !settings.handsRestoreProbe &&
                !settings.handsRenderCopyProbe &&
                !settings.handsPoseRestoreProbe &&
                !settings.handsPoseRenderCopyProbe,
            "no trace file or development probe should be implied");

    // Features that are off imply nothing.
    atb::config::Settings bare{};
    bare.features.renderUnlock = true;
    bare.features.presentationLimiter = true;
    atb::config::ApplyImpliedSettings(bare);
    Require(!bare.telemetryEnabled && !bare.motionTraceEnabled &&
                !bare.handsTraceEnabled && !bare.worldTraceEnabled &&
                !bare.mouseTraceEnabled,
            "the unlock and limiter alone should imply no layer");
    atb::config::Settings cursorOnly{};
    cursorOnly.features.mouseOverlay = true;
    atb::config::ApplyImpliedSettings(cursorOnly);
    Require(cursorOnly.telemetryEnabled && cursorOnly.motionTraceEnabled &&
                cursorOnly.mouseTraceEnabled && !cursorOnly.handsTraceEnabled &&
                !cursorOnly.worldTraceEnabled,
            "the cursor alone should imply only the mouse layer");

    std::cout << "release config tests passed\n";
    return EXIT_SUCCESS;
}
