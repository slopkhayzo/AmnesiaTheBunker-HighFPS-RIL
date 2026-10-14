#include "runtime_mapping.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    constexpr std::uint32_t swapRva = 0x005EC4F0U;
    constexpr std::array<std::uint8_t, 9> signature = {
        0x48, 0x8B, 0x49, 0x30, 0xE9, 0xD5, 0x36, 0x1E, 0x00,
    };
    constexpr std::uint32_t wantUpdateRva = 0x00555600U;
    constexpr std::array<std::uint8_t, 31> wantUpdateSignature = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x48, 0x83, 0xEC, 0x20, 0x8B, 0xF2, 0x48, 0x8B,
        0xD9, 0xE8, 0xC2, 0x09, 0xE4, 0xFF, 0x48,
    };
    constexpr std::uint32_t limitGetterRva = 0x0067F6B0U;
    constexpr std::array<std::uint8_t, 8> limitGetterSignature = {
        0x0F, 0xB6, 0x81, 0x88, 0x00, 0x00, 0x00, 0xC3,
    };
    constexpr std::uint32_t engineGlobalRva = 0x0096F888U;
    constexpr std::uint32_t sceneGlobalRva = 0x0096F890U;
    constexpr std::uint32_t updaterDispatchRva = 0x0039E900U;
    constexpr std::array<std::uint8_t, 20> updaterDispatchSignature = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48,
        0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54,
        0x41, 0x55, 0x41, 0x56,
    };
    constexpr std::uint32_t engineClockRva = 0x00395FE0U;
    constexpr std::array<std::uint8_t, 25> engineClockSignature = {
        0x48, 0x83, 0xEC, 0x28, 0xE8, 0x1F, 0x9B, 0x43,
        0x00, 0x48, 0x2B, 0x05, 0xC8, 0x58, 0x5B, 0x00,
        0x33, 0xD2, 0x48, 0x69, 0xC0, 0x40, 0x42, 0x0F,
        0x00,
    };
    constexpr std::uint32_t sceneListenerRva = 0x004253D0U;
    constexpr std::array<std::uint8_t, 24> sceneListenerSignature = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
        0x8B, 0xB9, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B,
    };
    constexpr std::uint32_t viewportSetCameraRva = 0x00706D00U;
    constexpr std::array<std::uint8_t, 23> viewportSetCameraSignature = {
        0x48, 0x89, 0x51, 0x18, 0x48, 0x8B, 0xC2, 0x48,
        0x85, 0xD2, 0x74, 0x0B, 0x8B, 0x51, 0x6C, 0x48,
        0x8B, 0xC8, 0xE9, 0x59, 0x09, 0xCC, 0xFF,
    };
    constexpr std::uint32_t cameraGetPositionRva = 0x0022C620U;
    constexpr std::array<std::uint8_t, 5> cameraGetPositionSignature = {
        0x48, 0x8D, 0x41, 0x10, 0xC3,
    };
    constexpr std::uint32_t cameraSetPitchRva = 0x003C73A0U;
    constexpr std::array<std::uint8_t, 16> cameraSetPitchSignature = {
        0xF3, 0x0F, 0x10, 0x51, 0x54, 0x0F, 0x57, 0xC0,
        0x0F, 0x2E, 0xD0, 0xF3, 0x0F, 0x11, 0x49, 0x48,
    };
    constexpr std::uint32_t cameraSetYawRva = 0x003C7620U;
    constexpr std::array<std::uint8_t, 16> cameraSetYawSignature = {
        0xF3, 0x0F, 0x10, 0x51, 0x5C, 0x0F, 0x57, 0xC0,
        0x0F, 0x2E, 0xD0, 0xF3, 0x0F, 0x11, 0x49, 0x4C,
    };
    constexpr std::uint32_t cameraSetRollRva = 0x003C7480U;
    constexpr std::array<std::uint8_t, 21> cameraSetRollSignature = {
        0xF3, 0x0F, 0x11, 0x49, 0x50, 0xC6, 0x81, 0xF1,
        0x06, 0x00, 0x00, 0x01, 0x66, 0xC7, 0x81, 0xF3,
        0x06, 0x00, 0x00, 0x01, 0x01,
    };
    constexpr std::uint32_t cameraSetPositionRva = 0x003C73F0U;
    constexpr std::array<std::uint8_t, 32> cameraSetPositionSignature = {
        0x8B, 0x02, 0x4C, 0x8D, 0x41, 0x10, 0x41, 0x89,
        0x00, 0x8B, 0x42, 0x04, 0x41, 0x89, 0x40, 0x04,
        0x8B, 0x42, 0x08, 0x49, 0x8B, 0xD0, 0xC6, 0x81,
        0xF1, 0x06, 0x00, 0x00, 0x01, 0x66, 0xC7, 0x81,
    };
    constexpr std::uint32_t cameraViewRva = 0x003C5DD0U;
    constexpr std::array<std::uint8_t, 32> cameraViewSignature = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9,
        0xF1, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9,
        0x74, 0x0F, 0xE8, 0xB9, 0x1D, 0x00, 0x00, 0x48,
        0x8D, 0x43, 0x78, 0x48, 0x83, 0xC4, 0x20, 0x5B,
    };
    constexpr std::uint32_t cameraMoveRva = 0x003C5C70U;
    constexpr std::array<std::uint8_t, 27> cameraMoveSignature = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
        0xD9, 0xE8, 0x32, 0x22, 0x00, 0x00, 0x48, 0x8D,
        0x83, 0x38, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4,
        0x20, 0x5B, 0xC3,
    };
    constexpr std::uint32_t cameraFrustumRva = 0x003C5AA0U;
    constexpr std::array<std::uint8_t, 32> cameraFrustumSignature = {
        0x40, 0x55, 0x48, 0x83, 0xEC, 0x70, 0x80, 0xB9,
        0xF4, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xE9,
        0x0F, 0x84, 0x66, 0x01, 0x00, 0x00, 0x4C, 0x89,
        0xA4, 0x24, 0x98, 0x00, 0x00, 0x00, 0x4C, 0x89,
    };
    constexpr std::uint32_t cameraExtendedFrustumRva = 0x003C5480U;
    constexpr std::array<std::uint8_t, 32> cameraExtendedFrustumSignature = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x58,
        0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x01,
    };
    constexpr std::uint32_t renderAllRva = 0x00424B20U;
    constexpr std::array<std::uint8_t, 32> renderAllSignature = {
        0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x60,
        0xFF, 0x05, 0x12, 0x77, 0x53, 0x00, 0x48, 0x89,
        0x68, 0x08, 0x33, 0xED, 0x48, 0x89, 0x78, 0x18,
        0x48, 0x8B, 0xF9, 0x0F, 0x29, 0x70, 0xE8, 0x0F,
    };
    constexpr std::uint32_t entitySetMatrixRva = 0x001988E0U;
    constexpr std::array<std::uint8_t, 32> entitySetMatrixSignature = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
        0x01, 0x48, 0x8B, 0xD9, 0xC6, 0x81, 0xCB, 0x03,
        0x00, 0x00, 0x01, 0xFF, 0x90, 0x70, 0x01, 0x00,
        0x00, 0x48, 0x8B, 0xCB, 0xE8, 0x7F, 0x51, 0x00,
    };
    constexpr std::uint32_t entityAttachmentRva = 0x0019BE50U;
    constexpr std::array<std::uint8_t, 32> entityAttachmentSignature = {
        0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xE8,
        0xFC, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x18, 0x04,
        0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89,
        0x90, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F,
    };
    constexpr std::uint32_t subMeshModelRva = 0x004460A0U;
    constexpr std::array<std::uint8_t, 16> subMeshModelSignature = {
        0x48, 0x83, 0xC1, 0x78, 0xE9, 0xC7, 0x7C, 0xF1,
        0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    };
    constexpr std::uint32_t subMeshPaletteGetterRva = 0x004ADAC0U;
    constexpr std::array<std::uint8_t, 20> subMeshPaletteGetterSignature = {
        0x48, 0x8B, 0x81, 0xB0, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC0,
        0x74, 0x08, 0x48, 0x8B, 0x80, 0x18, 0x03, 0x00, 0x00, 0xC3,
    };
    constexpr std::uint32_t subMeshSkinningRva = 0x004ADEB0U;
    constexpr std::array<std::uint8_t, 32> subMeshSkinningSignature = {
        0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
        0x00, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xB0,
        0x01, 0x00, 0x00, 0xE8, 0x38, 0x65, 0xE8, 0xFF,
        0x48, 0x83, 0xBB, 0xD8, 0x01, 0x00, 0x00, 0x00,
    };
    constexpr std::uint32_t meshPaletteRva = 0x00334400U;
    constexpr std::array<std::uint8_t, 32> meshPaletteSignature = {
        0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x08, 0x01,
        0x00, 0x00, 0x80, 0xB9, 0xB0, 0x01, 0x00, 0x00,
        0x00, 0x48, 0x8B, 0xF1, 0x75, 0x11, 0xE8, 0x45,
        0x99, 0x02, 0x00, 0x39, 0x86, 0xB4, 0x01, 0x00,
    };
    std::vector<std::uint8_t> mappedImage(engineGlobalRva + 0x100U, 0xCC);
    std::copy(signature.begin(), signature.end(),
              mappedImage.begin() + swapRva);
    std::copy(wantUpdateSignature.begin(), wantUpdateSignature.end(),
              mappedImage.begin() + wantUpdateRva);
    std::copy(limitGetterSignature.begin(), limitGetterSignature.end(),
              mappedImage.begin() + limitGetterRva);
    std::copy(updaterDispatchSignature.begin(), updaterDispatchSignature.end(),
              mappedImage.begin() + updaterDispatchRva);
    std::copy(engineClockSignature.begin(), engineClockSignature.end(),
              mappedImage.begin() + engineClockRva);
    std::copy(sceneListenerSignature.begin(), sceneListenerSignature.end(),
              mappedImage.begin() + sceneListenerRva);
    std::copy(viewportSetCameraSignature.begin(), viewportSetCameraSignature.end(),
              mappedImage.begin() + viewportSetCameraRva);
    std::copy(cameraGetPositionSignature.begin(), cameraGetPositionSignature.end(),
              mappedImage.begin() + cameraGetPositionRva);
    std::copy(cameraSetPitchSignature.begin(), cameraSetPitchSignature.end(),
              mappedImage.begin() + cameraSetPitchRva);
    std::copy(cameraSetYawSignature.begin(), cameraSetYawSignature.end(),
              mappedImage.begin() + cameraSetYawRva);
    std::copy(cameraSetRollSignature.begin(), cameraSetRollSignature.end(),
              mappedImage.begin() + cameraSetRollRva);
    std::copy(cameraSetPositionSignature.begin(), cameraSetPositionSignature.end(),
              mappedImage.begin() + cameraSetPositionRva);
    std::copy(cameraViewSignature.begin(), cameraViewSignature.end(),
              mappedImage.begin() + cameraViewRva);
    std::copy(cameraMoveSignature.begin(), cameraMoveSignature.end(),
              mappedImage.begin() + cameraMoveRva);
    std::copy(cameraFrustumSignature.begin(), cameraFrustumSignature.end(),
              mappedImage.begin() + cameraFrustumRva);
    std::copy(cameraExtendedFrustumSignature.begin(),
              cameraExtendedFrustumSignature.end(),
              mappedImage.begin() + cameraExtendedFrustumRva);
    std::copy(renderAllSignature.begin(), renderAllSignature.end(),
              mappedImage.begin() + renderAllRva);
    std::copy(entitySetMatrixSignature.begin(), entitySetMatrixSignature.end(),
              mappedImage.begin() + entitySetMatrixRva);
    std::copy(entityAttachmentSignature.begin(), entityAttachmentSignature.end(),
              mappedImage.begin() + entityAttachmentRva);
    std::copy(subMeshModelSignature.begin(), subMeshModelSignature.end(),
              mappedImage.begin() + subMeshModelRva);
    std::copy(subMeshPaletteGetterSignature.begin(),
              subMeshPaletteGetterSignature.end(),
              mappedImage.begin() + subMeshPaletteGetterRva);
    std::copy(subMeshSkinningSignature.begin(), subMeshSkinningSignature.end(),
              mappedImage.begin() + subMeshSkinningRva);
    std::copy(meshPaletteSignature.begin(), meshPaletteSignature.end(),
              mappedImage.begin() + meshPaletteRva);

    atb::target::HostImage image{};
    image.imageBase = reinterpret_cast<std::uintptr_t>(mappedImage.data());
    image.imageSize = static_cast<std::uint32_t>(mappedImage.size());
    image.textRva = 0;
    image.textSize = image.imageSize;
    const atb::target::KnownBuild build{
        atb::target::BuildId::gog_57301760292462330,
        L"test",
    };

    atb::mapping::TelemetryMap map{};
    std::wstring error;
    Require(atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.steadySwapWrapper == image.imageBase + swapRva &&
                map.logicTimerWantUpdate == image.imageBase + wantUpdateRva &&
                map.updaterDispatch == image.imageBase + updaterDispatchRva &&
                map.engineMicrosecondClock == image.imageBase + engineClockRva &&
                map.limitFpsGetter == image.imageBase + limitGetterRva &&
                map.engineGlobalPointer == image.imageBase + engineGlobalRva,
            "the exact frozen swap signature should resolve");
    error.clear();
    Require(atb::mapping::ResolveCameraTraceMap(image, build, map, error) &&
                map.sceneGlobalPointer == image.imageBase + sceneGlobalRva,
            "the exact frozen camera ownership/layout signatures should resolve");
    error.clear();
    Require(atb::mapping::ResolveCameraRuntimeMap(image, build, map, error) &&
                map.sceneRenderAll == image.imageBase + renderAllRva,
            "the exact frozen camera runtime signatures should resolve");
    error.clear();
    Require(atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == image.imageBase + entitySetMatrixRva &&
                map.entityUpdateAttachment ==
                    image.imageBase + entityAttachmentRva &&
                map.subMeshGetModelMatrix == image.imageBase + subMeshModelRva &&
                map.subMeshGetSkinningPalette ==
                    image.imageBase + subMeshPaletteGetterRva &&
                map.subMeshUpdateSkinning ==
                    image.imageBase + subMeshSkinningRva &&
                map.meshUpdatePalette == image.imageBase + meshPaletteRva,
            "the exact frozen hands trace signatures should resolve");

    mappedImage[entityAttachmentRva + 6] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == 0 &&
                map.entityUpdateAttachment == 0 &&
                map.subMeshGetModelMatrix == 0 &&
                map.subMeshGetSkinningPalette == 0 &&
                map.subMeshUpdateSkinning == 0 &&
                map.meshUpdatePalette == 0 && !error.empty(),
            "a modified attachment signature must fail closed");
    mappedImage[entityAttachmentRva + 6] ^= 1U;

    mappedImage[subMeshModelRva + 4] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == 0 &&
                map.entityUpdateAttachment == 0 &&
                map.subMeshGetModelMatrix == 0 &&
                map.subMeshGetSkinningPalette == 0 &&
                map.subMeshUpdateSkinning == 0 &&
                map.meshUpdatePalette == 0 && !error.empty(),
            "a modified submesh model-matrix signature must fail closed");
    mappedImage[subMeshModelRva + 4] ^= 1U;

    mappedImage[subMeshPaletteGetterRva + 15] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == 0 &&
                map.entityUpdateAttachment == 0 &&
                map.subMeshGetModelMatrix == 0 &&
                map.subMeshGetSkinningPalette == 0 &&
                map.subMeshUpdateSkinning == 0 &&
                map.meshUpdatePalette == 0 && !error.empty(),
            "a modified submesh palette-getter signature must fail closed");
    mappedImage[subMeshPaletteGetterRva + 15] ^= 1U;

    mappedImage[subMeshSkinningRva + 13] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == 0 &&
                map.entityUpdateAttachment == 0 &&
                map.subMeshGetModelMatrix == 0 &&
                map.subMeshGetSkinningPalette == 0 &&
                map.subMeshUpdateSkinning == 0 &&
                map.meshUpdatePalette == 0 && !error.empty(),
            "a modified submesh skinning signature must fail closed");
    mappedImage[subMeshSkinningRva + 13] ^= 1U;

    mappedImage[meshPaletteRva + 20] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveHandsTraceMap(image, build, map, error) &&
                map.entitySetMatrix == 0 &&
                map.entityUpdateAttachment == 0 &&
                map.subMeshGetModelMatrix == 0 &&
                map.subMeshGetSkinningPalette == 0 &&
                map.subMeshUpdateSkinning == 0 &&
                map.meshUpdatePalette == 0 && !error.empty(),
            "a modified mesh palette-update signature must fail closed");
    mappedImage[meshPaletteRva + 20] ^= 1U;

    mappedImage[renderAllRva + 8] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveCameraRuntimeMap(image, build, map, error) &&
                map.sceneRenderAll == 0 && !error.empty(),
            "a modified scene RenderAll signature must fail closed");
    mappedImage[renderAllRva + 8] ^= 1U;

    mappedImage[viewportSetCameraRva + 3] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveCameraTraceMap(image, build, map, error) &&
                map.sceneGlobalPointer == 0 && !error.empty(),
            "a modified viewport-camera layout must fail closed");
    mappedImage[viewportSetCameraRva + 3] ^= 1U;

    mappedImage[swapRva + 4] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.steadySwapWrapper == 0 && !error.empty(),
            "a modified swap signature should fail closed");

    mappedImage[swapRva + 4] ^= 1U;
    mappedImage[wantUpdateRva + 25] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.logicTimerWantUpdate == 0 && !error.empty(),
            "a modified logic-timer signature should fail closed");

    mappedImage[wantUpdateRva + 25] ^= 1U;
    mappedImage[updaterDispatchRva + 8] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.updaterDispatch == 0 && !error.empty(),
            "a modified updater-dispatch signature should fail closed");

    mappedImage[updaterDispatchRva + 8] ^= 1U;
    mappedImage[engineClockRva + 18] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.engineMicrosecondClock == 0 && !error.empty(),
            "a modified engine-clock signature should fail closed");

    mappedImage[engineClockRva + 18] ^= 1U;
    mappedImage[limitGetterRva + 3] ^= 1U;
    error.clear();
    Require(!atb::mapping::ResolveTelemetryMap(image, build, map, error) &&
                map.limitFpsGetter == 0 && !error.empty(),
            "a modified LimitFPS getter signature should fail closed");

    std::cout << "target-specific telemetry mapping tests passed\n";
    return EXIT_SUCCESS;
}
