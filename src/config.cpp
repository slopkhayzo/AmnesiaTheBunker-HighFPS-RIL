#include "config.h"

#include <Windows.h>

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string_view>

namespace atb::config {
namespace {

constexpr wchar_t kMissingValue[] = L"{A70B817E-B18A-4D19-ATB-MISSING}";

std::wstring TrimAndLower(std::wstring value) {
    const auto isNotSpace = [](const wchar_t ch) { return std::iswspace(ch) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), isNotSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), isNotSpace).base();
    if (first >= last) {
        return {};
    }
    std::wstring normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const wchar_t ch) {
                       return static_cast<wchar_t>(std::towlower(ch));
                   });
    return normalized;
}

bool ReadValue(const std::wstring& path,
               const wchar_t* section,
               const wchar_t* key,
               std::wstring& value) {
    wchar_t buffer[128]{};
    const DWORD copied = GetPrivateProfileStringW(
        section, key, kMissingValue, buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    if (copied == 0 || std::wstring_view(buffer) == kMissingValue) {
        return false;
    }
    value.assign(buffer, copied);
    return true;
}

bool ParseBool(const std::wstring& raw, bool& value) {
    const std::wstring normalized = TrimAndLower(raw);
    if (normalized == L"1" || normalized == L"true" || normalized == L"yes" ||
        normalized == L"on") {
        value = true;
        return true;
    }
    if (normalized == L"0" || normalized == L"false" || normalized == L"no" ||
        normalized == L"off") {
        value = false;
        return true;
    }
    return false;
}

bool ReadBool(const std::wstring& path,
              const wchar_t* section,
              const wchar_t* key,
              const bool defaultValue,
              bool& value,
              std::wstring& error) {
    std::wstring raw;
    if (!ReadValue(path, section, key, raw)) {
        value = defaultValue;
        return true;
    }
    if (ParseBool(raw, value)) {
        return true;
    }
    error = L"Invalid boolean value for [" + std::wstring(section) + L"] " + key + L": " + raw;
    return false;
}

bool ReadBoundedUnsigned(const std::wstring& path,
                         const wchar_t* section,
                         const wchar_t* key,
                         const unsigned int defaultValue,
                         const unsigned int minimum,
                         const unsigned int maximum,
                         unsigned int& value,
                         std::wstring& error) {
    std::wstring raw;
    if (!ReadValue(path, section, key, raw)) {
        value = defaultValue;
        return true;
    }
    const std::wstring normalized = TrimAndLower(raw);
    if (normalized.empty() || normalized.front() == L'-') {
        error = L"Invalid unsigned value for [" + std::wstring(section) + L"] " + key +
                L": " + raw;
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(normalized.c_str(), &end, 10);
    if (errno == ERANGE || end == normalized.c_str() || *end != L'\0' ||
        parsed < minimum || parsed > maximum) {
        error = L"Out-of-range value for [" + std::wstring(section) + L"] " + key +
                L" (expected " + std::to_wstring(minimum) + L".." +
                std::to_wstring(maximum) + L"): " + raw;
        return false;
    }
    value = static_cast<unsigned int>(parsed);
    return true;
}

} // namespace

LoadResult Load(const std::wstring& path) {
    LoadResult result{};
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = L"Configuration file not found: " + path;
        return result;
    }

    auto& s = result.settings;
    auto& f = s.features;
    if (!ReadBool(path, L"General", L"Enabled", true, s.enabled, result.error) ||
        !ReadBool(path, L"General", L"Diagnostics", true, s.diagnostics, result.error) ||
        !ReadBool(path, L"Telemetry", L"Enabled", false, s.telemetryEnabled, result.error) ||
        !ReadBool(path, L"Telemetry", L"MotionTrace", false, s.motionTraceEnabled, result.error) ||
        !ReadBool(path, L"Telemetry", L"CameraRestoreProbe", false,
                  s.cameraRestoreProbe, result.error) ||
        !ReadBool(path, L"Telemetry", L"HandsTrace", false,
                  s.handsTraceEnabled, result.error) ||
        !ReadBool(path, L"Telemetry", L"HandsRenderCopyProbe", false,
                  s.handsRenderCopyProbe, result.error) ||
        !ReadBool(path, L"Telemetry", L"HandsPoseRestoreProbe", false,
                  s.handsPoseRestoreProbe, result.error) ||
        !ReadBool(path, L"Telemetry", L"HandsPoseRenderCopyProbe", false,
                  s.handsPoseRenderCopyProbe, result.error) ||
        !ReadBool(path, L"Telemetry", L"HandsRestoreProbe", false,
                  s.handsRestoreProbe, result.error) ||
        !ReadBool(path, L"Telemetry", L"WorldTrace", false,
                  s.worldTraceEnabled, result.error) ||
        !ReadBool(path, L"Telemetry", L"MouseTrace", false,
                  s.mouseTraceEnabled, result.error) ||
        !ReadBool(path, L"Telemetry", L"FrameTrace", false,
                  s.frameTraceEnabled, result.error) ||
        !ReadBoundedUnsigned(path, L"Telemetry", L"ReportIntervalSeconds", 5, 1, 60,
                             s.telemetryReportIntervalSeconds, result.error) ||
        !ReadBool(path, L"Features", L"RenderUnlock", false, f.renderUnlock, result.error) ||
        !ReadBool(path, L"Features", L"PresentationLimiter", false,
                  f.presentationLimiter, result.error) ||
        !ReadBool(path, L"Features", L"CameraInterpolation", false,
                  f.cameraInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"HandsRootInterpolation", false,
                  f.handsRootInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"WorldRootInterpolation", false,
                  f.worldRootInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"HandsPoseInterpolation", false,
                  f.handsPoseInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"WorldPoseInterpolation", false,
                  f.worldPoseInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"RagdollPoseInterpolation", false,
                  f.ragdollPoseInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"ParticleInterpolation", false,
                  f.particleInterpolation, result.error) ||
        !ReadBool(path, L"Features", L"ImageTrailTiming", false,
                  f.imageTrailTiming, result.error) ||
        !ReadBool(path, L"Features", L"MouseOverlay", false, f.mouseOverlay, result.error) ||
        !ReadBool(path, L"Features", L"MousePreview", false, f.mousePreview,
                  result.error) ||
        !ReadBoundedUnsigned(path, L"Timing", L"TargetFPS", 0, 0, 1000,
                             s.targetFps, result.error) ||
        !ReadBoundedUnsigned(path, L"Timing", L"GameplayRateOverride", 0, 0, 1000,
                             s.gameplayRateOverride, result.error) ||
        !ReadBoundedUnsigned(path, L"Timing", L"FramePacingEvenness", 0, 0, 100,
                             s.framePacingEvenness, result.error)) {
        return result;
    }

    result.ok = true;
    return result;
}

void ApplyImpliedSettings(Settings& settings) noexcept {
    const Features& f = settings.features;
    const bool hands = f.handsRootInterpolation || f.handsPoseInterpolation;
    const bool world = f.worldRootInterpolation || f.worldPoseInterpolation;
    const bool mouse = f.mousePreview || f.mouseOverlay;
    if (f.cameraInterpolation || hands || world || mouse ||
        f.particleInterpolation) {
        settings.telemetryEnabled = true;
        settings.motionTraceEnabled = true;
    }
    if (hands || world) {
        settings.handsTraceEnabled = true;
    }
    if (world) {
        settings.worldTraceEnabled = true;
    }
    if (mouse) {
        settings.mouseTraceEnabled = true;
    }
}

bool AnyBehavioralFeatureEnabled(const Features& f) noexcept {
    return f.renderUnlock || f.presentationLimiter || f.cameraInterpolation ||
           f.handsRootInterpolation || f.worldRootInterpolation ||
           f.handsPoseInterpolation || f.worldPoseInterpolation ||
           f.ragdollPoseInterpolation || f.particleInterpolation ||
           f.imageTrailTiming || f.mouseOverlay || f.mousePreview;
}

bool AnyUnsupportedBehavioralFeatureEnabled(const Features& f) noexcept {
    return f.ragdollPoseInterpolation || f.imageTrailTiming;
}

bool AnyRuntimeMappingRequested(const Settings& settings) noexcept {
    return settings.telemetryEnabled || settings.motionTraceEnabled ||
           settings.cameraRestoreProbe || settings.handsTraceEnabled ||
           settings.handsRenderCopyProbe ||
           settings.handsPoseRestoreProbe ||
           settings.handsPoseRenderCopyProbe ||
           settings.handsRestoreProbe || settings.worldTraceEnabled ||
           settings.mouseTraceEnabled ||
           settings.gameplayRateOverride != 0 ||
           AnyBehavioralFeatureEnabled(settings.features);
}

} // namespace atb::config
