#pragma once

#include "target_mapping.h"

#include <cstdint>
#include <string>

namespace atb::mapping {

struct TelemetryMap {
    std::uintptr_t steadySwapWrapper = 0;
    std::uintptr_t logicTimerWantUpdate = 0;
    std::uintptr_t updaterDispatch = 0;
    std::uintptr_t engineMicrosecondClock = 0;
    std::uintptr_t limitFpsGetter = 0;
    std::uintptr_t engineGlobalPointer = 0;
    std::uintptr_t sceneGlobalPointer = 0;
    std::uintptr_t sceneRenderAll = 0;
    std::uintptr_t entitySetMatrix = 0;
    std::uintptr_t entityUpdateAttachment = 0;
    std::uintptr_t subMeshGetModelMatrix = 0;
    std::uintptr_t subMeshGetSkinningPalette = 0;
    std::uintptr_t subMeshUpdateSkinning = 0;
    std::uintptr_t meshUpdatePalette = 0;
    std::uintptr_t engineSetUpdatesPerSec = 0;
    std::uintptr_t renderableGetMatrixUpdateCount = 0;
    std::uintptr_t lightSpotGetViewProjMatrix = 0;
    std::uintptr_t lightSpotGetProjectionMatrix = 0;
    std::uintptr_t lightProjectionBiasMatrix = 0;
    std::uintptr_t shadowMapNeedsUpdate = 0;
    std::uintptr_t cameraGetViewMatrix = 0;
    std::uintptr_t lightSpotGetViewMatrix = 0;
    std::uintptr_t lightSpotGetFrustum = 0;
    std::uintptr_t imGuiDraw = 0;
    std::uintptr_t imGuiDoMouse = 0;
    std::uintptr_t imGuiEnd = 0;
    std::uintptr_t imGuiSendMousePosition = 0;
    std::uintptr_t imGuiSendMouseVirtualPosition = 0;
    std::uintptr_t particleUpdateGraphics = 0;
};

[[nodiscard]] bool ResolveTelemetryMap(const target::HostImage& image,
                                       const target::KnownBuild& build,
                                       TelemetryMap& map,
                                       std::wstring& error) noexcept;
[[nodiscard]] bool ResolveCameraTraceMap(const target::HostImage& image,
                                         const target::KnownBuild& build,
                                         TelemetryMap& map,
                                         std::wstring& error) noexcept;
[[nodiscard]] bool ResolveCameraRuntimeMap(const target::HostImage& image,
                                           const target::KnownBuild& build,
                                           TelemetryMap& map,
                                           std::wstring& error) noexcept;
[[nodiscard]] bool ResolveHandsTraceMap(const target::HostImage& image,
                                        const target::KnownBuild& build,
                                        TelemetryMap& map,
                                        std::wstring& error) noexcept;
[[nodiscard]] bool ResolveUpdateRateMap(const target::HostImage& image,
                                        const target::KnownBuild& build,
                                        TelemetryMap& map,
                                        std::wstring& error) noexcept;
[[nodiscard]] bool ResolveWorldMap(const target::HostImage& image,
                                   const target::KnownBuild& build,
                                   TelemetryMap& map,
                                   std::wstring& error) noexcept;
[[nodiscard]] bool ResolveGuiMap(const target::HostImage& image,
                                 const target::KnownBuild& build,
                                 TelemetryMap& map,
                                 std::wstring& error) noexcept;
[[nodiscard]] bool ResolveParticleMap(const target::HostImage& image,
                                      const target::KnownBuild& build,
                                      TelemetryMap& map,
                                      std::wstring& error) noexcept;

} // namespace atb::mapping
