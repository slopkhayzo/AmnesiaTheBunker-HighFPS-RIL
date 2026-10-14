#include "runtime_mapping.h"
#include "sha256.h"
#include "target_mapping.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int Fail(const std::wstring& message) {
    std::wcerr << L"FAIL: " << message << L'\n';
    return EXIT_FAILURE;
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) {
        return Fail(L"usage: installed_target_tests <AmnesiaTheBunker.exe>");
    }
    const std::wstring path(arguments[1]);
    const atb::crypto::Sha256Result hash = atb::crypto::HashFile(path);
    if (!hash.ok) {
        return Fail(L"could not hash installed target: " + hash.error);
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Fail(L"CreateFileW failed with error " +
                    std::to_wstring(GetLastError()));
    }
    const HANDLE mapping = CreateFileMappingW(
        file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    if (mapping == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return Fail(L"CreateFileMappingW(SEC_IMAGE_NO_EXECUTE) failed with error " +
                    std::to_wstring(error));
    }
    const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        CloseHandle(file);
        return Fail(L"MapViewOfFile failed with error " + std::to_wstring(error));
    }

    atb::target::HostImage image{};
    std::wstring error;
    const bool inspected = atb::target::InspectMappedPe64(
        reinterpret_cast<HMODULE>(const_cast<void*>(view)), image, error);
    atb::target::KnownBuild build{};
    const bool matched = inspected &&
        atb::target::MatchKnownBuild(image, hash.digest, build, error);
    atb::mapping::TelemetryMap telemetryMap{};
    const bool resolved = matched &&
        atb::mapping::ResolveTelemetryMap(image, build, telemetryMap, error);
    const bool cameraResolved = resolved &&
        atb::mapping::ResolveCameraTraceMap(image, build, telemetryMap, error);
    const bool cameraRuntimeResolved = cameraResolved &&
        atb::mapping::ResolveCameraRuntimeMap(image, build, telemetryMap, error);
    const bool handsTraceResolved = cameraRuntimeResolved &&
        atb::mapping::ResolveHandsTraceMap(image, build, telemetryMap, error);

    const bool updateRateResolved = handsTraceResolved &&
        atb::mapping::ResolveUpdateRateMap(image, build, telemetryMap,
                                           error) &&
        atb::mapping::ResolveWorldMap(image, build, telemetryMap, error) &&
        atb::mapping::ResolveGuiMap(image, build, telemetryMap, error) &&
        atb::mapping::ResolveParticleMap(image, build, telemetryMap, error);

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(file);

    if (!updateRateResolved) {
        return Fail(error);
    }
    std::wcout << L"installed target identity, telemetry, camera-runtime, and hands-trace signatures verified: "
               << build.name << L'\n';
    return EXIT_SUCCESS;
}
