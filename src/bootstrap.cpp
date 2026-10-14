#include "bootstrap.h"

#include "config.h"
#include "logger.h"
#include "plugin_version.h"
#include "runtime_mapping.h"
#include "sha256.h"
#include "target_mapping.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace atb {
namespace {

enum class BootstrapState : unsigned int {
    dormant,
    starting,
    disabled,
    rejected,
    readyWithoutHooks,
    readyWithTelemetry,
};

std::atomic<BootstrapState> g_state{BootstrapState::dormant};

std::wstring ModulePath(const HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::wstring ReplaceExtension(const std::wstring& path,
                              const std::wstring_view extension) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
        return path + std::wstring(extension);
    }
    return path.substr(0, dot) + std::wstring(extension);
}

std::wstring LocalAppDataLogPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD copied = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return {};
    }
    std::wstring directory(buffer.data(), copied);
    directory.append(L"\\AmnesiaTheBunker.FPSFix");
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory + L"\\AmnesiaTheBunker.FPSFix.log";
}

std::wstring Hex(const std::uint64_t value, const unsigned int width) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(width)
           << std::setfill(L'0') << value;
    return stream.str();
}

void LogHostImage(const target::HostImage& image) {
    log::Write(log::Level::info,
               L"Host PE32+: machine=" + Hex(image.machine, 4) +
                   L", imageBase=" + Hex(image.imageBase, 16) +
                   L", imageSize=" + Hex(image.imageSize, 8) +
                   L", timestamp=" + Hex(image.timeDateStamp, 8) +
                   L", entryRva=" + Hex(image.entryPointRva, 8) +
                   L", sections=" + std::to_wstring(image.sectionCount));
    log::Write(log::Level::debug,
               L"Host sections: .text RVA=" + Hex(image.textRva, 8) +
                   L", size=" + Hex(image.textSize, 8) +
                   L", imports RVA=" + Hex(image.importDirectoryRva, 8) +
                   L", size=" + Hex(image.importDirectorySize, 8));
}

} // namespace

DWORD WINAPI BootstrapThread(void* moduleParameter) {
    g_state.store(BootstrapState::starting, std::memory_order_release);
    const HMODULE pluginModule = static_cast<HMODULE>(moduleParameter);
    const std::wstring pluginPath = ModulePath(pluginModule);
    if (pluginPath.empty()) {
        OutputDebugStringW(
            L"[AmnesiaTheBunker.FPSFix] Could not resolve plugin module path.\n");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 1;
    }

    const std::wstring iniPath = ReplaceExtension(pluginPath, L".ini");
    const std::wstring siblingLogPath = ReplaceExtension(pluginPath, L".log");
    // One log per launch; the session before is kept beside it.
    static_cast<void>(MoveFileExW(
        siblingLogPath.c_str(),
        ReplaceExtension(pluginPath, L".previous.log").c_str(),
        MOVEFILE_REPLACE_EXISTING));
    if (!log::Initialize(siblingLogPath)) {
        const std::wstring fallback = LocalAppDataLogPath();
        if (fallback.empty() || !log::Initialize(fallback)) {
            OutputDebugStringW(
                L"[AmnesiaTheBunker.FPSFix] Could not open a diagnostic log.\n");
        } else {
            log::Write(log::Level::warning,
                       L"Could not write sibling log; using: " + fallback);
        }
    }

    log::Write(log::Level::info,
               std::wstring(AMNESIA_BUNKER_FPS_FIX_NAME) + L" " +
                    AMNESIA_BUNKER_FPS_FIX_VERSION + L" bootstrap starting (x64, PID " +
                    std::to_wstring(GetCurrentProcessId()) + L")");
    log::Write(log::Level::debug, L"Plugin path: " + pluginPath);
    log::Write(log::Level::debug, L"INI path: " + iniPath);

    config::LoadResult loaded = config::Load(iniPath);
    if (!loaded.ok) {
        log::Write(log::Level::error, loaded.error);
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 2;
    }
    log::SetDiagnosticsEnabled(loaded.settings.diagnostics);
    if (!loaded.settings.enabled) {
        log::Write(log::Level::info,
                   L"Plugin disabled by [General] Enabled=false.");
        g_state.store(BootstrapState::disabled, std::memory_order_release);
        return 0;
    }
    config::ApplyImpliedSettings(loaded.settings);
    if (loaded.settings.features.presentationLimiter &&
        loaded.settings.targetFps == 0) {
        // TargetFPS = 0 means the refresh rate of the primary display.
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1) {
            loaded.settings.targetFps =
                mode.dmDisplayFrequency > 1000 ? 1000U
                                               : mode.dmDisplayFrequency;
            log::Write(log::Level::info,
                       L"TargetFPS=0: using the display refresh rate, " +
                           std::to_wstring(loaded.settings.targetFps) +
                           L" FPS.");
        } else {
            loaded.settings.features.presentationLimiter = false;
            log::Write(log::Level::warning,
                       L"TargetFPS=0 and the display refresh rate could not "
                       L"be read: the frame limiter and frame pacing are "
                       L"off. Set [Timing] TargetFPS to use them.");
        }
    }

    std::wstring hostHash;
    const std::wstring executablePath = ModulePath(nullptr);
    if (!executablePath.empty()) {
        log::Write(log::Level::info, L"Host path: " + executablePath);
        const crypto::Sha256Result hash = crypto::HashFile(executablePath);
        if (hash.ok) {
            hostHash = hash.digest;
            log::Write(log::Level::info, L"Host SHA-256: " + hash.digest);
        } else {
            log::Write(log::Level::warning,
                       L"Host SHA-256 unavailable: " + hash.error);
        }
    }

    target::HostImage host{};
    std::wstring hostError;
    if (!target::InspectMappedPe64(GetModuleHandleW(nullptr), host, hostError)) {
        log::Write(log::Level::error, L"Host inspection failed: " + hostError);
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 3;
    }
    LogHostImage(host);

    if (config::AnyRuntimeMappingRequested(loaded.settings)) {
        target::KnownBuild build{};
        std::wstring buildError;
        if (!target::MatchKnownBuild(host, hostHash, build, buildError)) {
            log::Write(log::Level::error,
                       L"Target identity rejected: " + buildError);
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 4;
        }
        log::Write(build.verified ? log::Level::info : log::Level::warning,
                   std::wstring(L"Host is ") + build.name + L".");

        const bool unsupportedRequest =
            config::AnyUnsupportedBehavioralFeatureEnabled(
                loaded.settings.features);
        if (unsupportedRequest) {
            log::Write(log::Level::error,
                       L"The requested behavioral feature is not "
                       L"implemented in this milestone.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }

        if (loaded.settings.features.presentationLimiter &&
            (!loaded.settings.features.renderUnlock ||
             loaded.settings.targetFps == 0)) {
            log::Write(log::Level::error,
                       L"PresentationLimiter requires RenderUnlock=true and a "
                       L"non-zero [Timing] TargetFPS.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.motionTraceEnabled &&
            !loaded.settings.telemetryEnabled) {
            log::Write(log::Level::error,
                       L"[Telemetry] MotionTrace requires Telemetry Enabled=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.cameraRestoreProbe &&
            (!loaded.settings.telemetryEnabled ||
             !loaded.settings.motionTraceEnabled)) {
            log::Write(log::Level::error,
                       L"[Telemetry] CameraRestoreProbe requires both "
                       L"Enabled=true and MotionTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsTraceEnabled &&
            (!loaded.settings.telemetryEnabled ||
             !loaded.settings.motionTraceEnabled)) {
            log::Write(log::Level::error,
                       L"[Telemetry] HandsTrace requires both Enabled=true "
                       L"and MotionTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsRenderCopyProbe &&
            !loaded.settings.handsTraceEnabled) {
            log::Write(log::Level::error,
                       L"[Telemetry] HandsRenderCopyProbe requires "
                       L"HandsTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsPoseRestoreProbe &&
            !loaded.settings.handsTraceEnabled) {
            log::Write(log::Level::error,
                       L"[Telemetry] HandsPoseRestoreProbe requires "
                       L"HandsTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsPoseRenderCopyProbe &&
            !loaded.settings.handsTraceEnabled) {
            log::Write(log::Level::error,
                       L"[Telemetry] HandsPoseRenderCopyProbe requires "
                       L"HandsTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.handsRootInterpolation &&
            (!loaded.settings.handsTraceEnabled ||
             !loaded.settings.features.cameraInterpolation)) {
            log::Write(log::Level::error,
                       L"HandsRootInterpolation requires HandsTrace=true "
                       L"and CameraInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.handsPoseInterpolation &&
            (!loaded.settings.handsTraceEnabled ||
             !loaded.settings.features.cameraInterpolation ||
             !loaded.settings.features.handsRootInterpolation)) {
            log::Write(log::Level::error,
                       L"HandsPoseInterpolation requires HandsTrace=true, "
                       L"CameraInterpolation=true, and "
                       L"HandsRootInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.gameplayRateOverride != 0 &&
            ((loaded.settings.gameplayRateOverride != 60 &&
              loaded.settings.gameplayRateOverride != 120) ||
             !loaded.settings.features.renderUnlock)) {
            log::Write(log::Level::error,
                       L"GameplayRateOverride must be 0, 60, or 120 and "
                       L"requires RenderUnlock=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.mousePreview &&
            (!loaded.settings.mouseTraceEnabled ||
             !loaded.settings.features.worldRootInterpolation)) {
            log::Write(log::Level::error,
                       L"MousePreview requires MouseTrace=true and "
                       L"WorldRootInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.particleInterpolation &&
            !loaded.settings.features.handsRootInterpolation) {
            log::Write(log::Level::error,
                       L"ParticleInterpolation requires "
                       L"HandsRootInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.mouseOverlay &&
            !loaded.settings.mouseTraceEnabled) {
            log::Write(log::Level::error,
                       L"MouseOverlay requires MouseTrace=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.worldPoseInterpolation &&
            !loaded.settings.features.worldRootInterpolation) {
            log::Write(log::Level::error,
                       L"WorldPoseInterpolation requires "
                       L"WorldRootInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if ((loaded.settings.worldTraceEnabled &&
             !loaded.settings.handsTraceEnabled) ||
            (loaded.settings.features.worldRootInterpolation &&
             (!loaded.settings.worldTraceEnabled ||
              !loaded.settings.features.cameraInterpolation ||
              !loaded.settings.features.handsRootInterpolation))) {
            log::Write(log::Level::error,
                       L"WorldTrace requires HandsTrace=true; "
                       L"WorldRootInterpolation also requires "
                       L"WorldTrace=true, CameraInterpolation=true, and "
                       L"HandsRootInterpolation=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsPoseRestoreProbe &&
            loaded.settings.features.handsPoseInterpolation) {
            log::Write(log::Level::error,
                       L"HandsPoseRestoreProbe and HandsPoseInterpolation are "
                       L"mutually exclusive.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsPoseRenderCopyProbe &&
            (loaded.settings.handsPoseRestoreProbe ||
             loaded.settings.features.handsPoseInterpolation)) {
            log::Write(log::Level::error,
                       L"HandsPoseRenderCopyProbe, HandsPoseRestoreProbe, and "
                       L"HandsPoseInterpolation are mutually exclusive.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsRenderCopyProbe &&
            loaded.settings.features.handsRootInterpolation) {
            log::Write(log::Level::error,
                       L"HandsRenderCopyProbe and HandsRootInterpolation are "
                       L"mutually exclusive.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.handsRestoreProbe) {
            log::Write(log::Level::error,
                       L"[Telemetry] HandsRestoreProbe is retired because "
                       L"entity lifetime is not stable across RenderAll.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.features.cameraInterpolation &&
            (!loaded.settings.telemetryEnabled ||
             !loaded.settings.features.renderUnlock)) {
            log::Write(log::Level::error,
                       L"CameraInterpolation requires Telemetry Enabled=true "
                       L"and RenderUnlock=true.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }
        if (loaded.settings.cameraRestoreProbe &&
            loaded.settings.features.cameraInterpolation) {
            log::Write(log::Level::error,
                       L"CameraRestoreProbe and CameraInterpolation are "
                       L"mutually exclusive.");
            log::Write(log::Level::error,
                       L"Fail-closed: no engine hooks were installed.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 5;
        }

        if (loaded.settings.telemetryEnabled ||
            loaded.settings.features.renderUnlock ||
            loaded.settings.features.presentationLimiter) {
            mapping::TelemetryMap map{};
            std::wstring mappingError;
            if (!mapping::ResolveTelemetryMap(host, build, map, mappingError)) {
                log::Write(log::Level::error,
                           L"Telemetry mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected, std::memory_order_release);
                return 6;
            }
            if (loaded.settings.features.particleInterpolation &&
                !mapping::ResolveParticleMap(host, build, map, mappingError)) {
                log::Write(log::Level::error,
                           L"Particle mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if (loaded.settings.features.mouseOverlay &&
                !mapping::ResolveGuiMap(host, build, map, mappingError)) {
                log::Write(log::Level::error,
                           L"GUI mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if (loaded.settings.features.worldRootInterpolation &&
                !mapping::ResolveWorldMap(host, build, map, mappingError)) {
                log::Write(log::Level::error,
                           L"World-object mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if (loaded.settings.gameplayRateOverride != 0 &&
                !mapping::ResolveUpdateRateMap(host, build, map,
                                               mappingError)) {
                log::Write(log::Level::error,
                           L"Update-rate mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if (loaded.settings.motionTraceEnabled &&
                !mapping::ResolveCameraTraceMap(host, build, map,
                                                mappingError)) {
                log::Write(log::Level::error,
                           L"Camera-trace mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if ((loaded.settings.cameraRestoreProbe ||
                 loaded.settings.features.cameraInterpolation ||
                 loaded.settings.handsTraceEnabled) &&
                !mapping::ResolveCameraRuntimeMap(host, build, map,
                                                  mappingError)) {
                log::Write(log::Level::error,
                           L"Camera-runtime mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            if (loaded.settings.handsTraceEnabled &&
                !mapping::ResolveHandsTraceMap(host, build, map,
                                               mappingError)) {
                log::Write(log::Level::error,
                           L"Hands-trace mapping rejected: " + mappingError);
                log::Write(log::Level::error,
                           L"Fail-closed: no engine hooks were installed.");
                g_state.store(BootstrapState::rejected,
                              std::memory_order_release);
                return 6;
            }
            const telemetry::Options runtimeOptions{
                loaded.settings.telemetryEnabled,
                loaded.settings.motionTraceEnabled,
                loaded.settings.cameraRestoreProbe,
                loaded.settings.features.cameraInterpolation,
                loaded.settings.handsTraceEnabled,
                loaded.settings.handsRenderCopyProbe,
                loaded.settings.handsPoseRestoreProbe,
                loaded.settings.handsPoseRenderCopyProbe,
                loaded.settings.features.handsRootInterpolation,
                loaded.settings.features.handsPoseInterpolation,
                loaded.settings.handsRestoreProbe,
                loaded.settings.features.renderUnlock,
                loaded.settings.features.presentationLimiter,
                loaded.settings.targetFps,
                loaded.settings.telemetryReportIntervalSeconds,
                loaded.settings.worldTraceEnabled,
                loaded.settings.features.worldRootInterpolation,
                loaded.settings.gameplayRateOverride,
                loaded.settings.features.worldPoseInterpolation,
                loaded.settings.mouseTraceEnabled,
                loaded.settings.features.mousePreview,
                loaded.settings.frameTraceEnabled,
                loaded.settings.framePacingEvenness,
                loaded.settings.features.mouseOverlay,
                loaded.settings.features.particleInterpolation,
            };
            std::wstring telemetryError;
            if (!telemetry::Install(map, runtimeOptions, telemetryError)) {
                log::Write(log::Level::error,
                           L"Telemetry installation failed: " + telemetryError);
                log::Write(log::Level::error,
                           L"Fail-closed: no behavioral hooks were installed.");
                g_state.store(BootstrapState::rejected, std::memory_order_release);
                return 7;
            }
            log::Write(log::Level::info,
                       loaded.settings.features.presentationLimiter
                           ? L"Bootstrap ready with opt-in RenderUnlock and "
                             L"PresentationLimiter."
                           : loaded.settings.features.renderUnlock
                           ? L"Bootstrap ready with opt-in RenderUnlock."
                           : L"Bootstrap ready in telemetry-only mode.");
            g_state.store(BootstrapState::readyWithTelemetry,
                          std::memory_order_release);
            return 0;
        }
    }

    log::Write(log::Level::info,
               L"Bootstrap ready. No runtime mappings or hooks were requested.");
    g_state.store(BootstrapState::readyWithoutHooks, std::memory_order_release);
    return 0;
}

} // namespace atb
