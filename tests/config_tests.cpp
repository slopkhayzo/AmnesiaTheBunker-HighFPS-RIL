#include "config.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    const bool poseRestore = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--pose-restore";
    const bool poseInterpolation = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--pose-interpolation";
    const bool poseRenderCopy = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--pose-render-copy";
    const bool worldTrace = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--world-trace";
    const bool play = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--play";
    const bool worldInterpolation = play || (argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"--world-interpolation");
    const bool world = worldTrace || worldInterpolation;
    if (argumentCount != 2 && !poseRestore && !poseInterpolation &&
        !poseRenderCopy && !world) {
        std::cerr << "usage: config_tests <profile.ini> "
                     "[--pose-restore|--pose-render-copy|"
                     "--pose-interpolation]\n";
        return EXIT_FAILURE;
    }
    const atb::config::LoadResult loaded = atb::config::Load(arguments[1]);
    Require(loaded.ok, "profile should parse");
    const auto& settings = loaded.settings;
    Require(settings.enabled && settings.telemetryEnabled &&
                settings.motionTraceEnabled && settings.handsTraceEnabled,
            "profile should enable required telemetry");
    Require(!settings.handsRenderCopyProbe &&
                settings.handsPoseRestoreProbe == poseRestore &&
                settings.handsPoseRenderCopyProbe == poseRenderCopy &&
                !settings.handsRestoreProbe,
            "profile should select only the expected hands pose probe");
    Require(settings.features.renderUnlock &&
                settings.features.presentationLimiter &&
                settings.features.cameraInterpolation &&
                settings.features.handsRootInterpolation &&
                settings.features.handsPoseInterpolation ==
                    (poseInterpolation || world),
            "profile should enable its runtime prerequisites");
    Require(settings.worldTraceEnabled == world &&
                settings.features.worldRootInterpolation == worldInterpolation,
            "profile should select only the expected world-object mode");
    Require(settings.gameplayRateOverride == (worldInterpolation ? 60U : 0U),
            "only the world-interpolation profile opts into the 60 Hz rate");
    Require(settings.mouseTraceEnabled == play &&
                settings.features.mousePreview == play,
            "only the play profile enables the mouse trace and preview");
    Require(settings.features.worldPoseInterpolation == play,
            "only the play profile enables world pose interpolation");
    Require(settings.targetFps == (play ? 350U : 240U),
            "profile should retain its presentation target");
    Require(atb::config::AnyRuntimeMappingRequested(settings),
            "hands interpolation profile should request runtime mappings");
    Require(!atb::config::AnyUnsupportedBehavioralFeatureEnabled(
                settings.features),
            "implemented hands interpolation must not be rejected as unsupported");
    std::cout << (poseRestore ? "pose restore profile tests passed\n" :
                  poseRenderCopy ? "pose render-copy profile tests passed\n" :
                  poseInterpolation ? "pose interpolation profile tests passed\n" :
                               "config tests passed\n");
    return EXIT_SUCCESS;
}
