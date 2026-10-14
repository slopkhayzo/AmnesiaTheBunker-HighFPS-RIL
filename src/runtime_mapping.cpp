#include "runtime_mapping.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atb::mapping {
namespace {

constexpr std::uint32_t kSteadySwapWrapperRva = 0x005EC4F0U;
constexpr std::array<std::uint8_t, 9> kSteadySwapWrapperSignature = {
    0x48, 0x8B, 0x49, 0x30, 0xE9, 0xD5, 0x36, 0x1E, 0x00,
};
constexpr std::uint32_t kLogicTimerWantUpdateRva = 0x00555600U;
constexpr std::array<std::uint8_t, 31> kLogicTimerWantUpdateSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x8B, 0xF2, 0x48, 0x8B,
    0xD9, 0xE8, 0xC2, 0x09, 0xE4, 0xFF, 0x48,
};
constexpr std::uint32_t kUpdaterDispatchRva = 0x0039E900U;
constexpr std::array<std::uint8_t, 20> kUpdaterDispatchSignature = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48,
    0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54,
    0x41, 0x55, 0x41, 0x56,
};
constexpr std::uint32_t kEngineMicrosecondClockRva = 0x00395FE0U;
constexpr std::array<std::uint8_t, 25> kEngineMicrosecondClockSignature = {
    0x48, 0x83, 0xEC, 0x28, 0xE8, 0x1F, 0x9B, 0x43,
    0x00, 0x48, 0x2B, 0x05, 0xC8, 0x58, 0x5B, 0x00,
    0x33, 0xD2, 0x48, 0x69, 0xC0, 0x40, 0x42, 0x0F,
    0x00,
};
// cEngine::SetUpdatesPerSec(rate, catchUpDivisor): loads the logic timer
// from engine+0x48 and tail-calls the timer's own setter at RVA 0x00555530.
constexpr std::uint32_t kEngineSetUpdatesPerSecRva = 0x0039DAC0U;
constexpr std::array<std::uint8_t, 9> kEngineSetUpdatesPerSecSignature = {
    0x48, 0x8B, 0x49, 0x48, 0xE9, 0x67, 0x7A, 0x1B, 0x00,
};
constexpr std::uint32_t kLogicTimerSetUpdatesPerSecRva = 0x00555530U;
constexpr std::array<std::uint8_t, 32> kLogicTimerSetUpdatesPerSecSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x57, 0x41,
    0x56, 0x48, 0x83, 0xEC, 0x20, 0xF2, 0x0F, 0x10,
    0x0D, 0x9B, 0xF0, 0x2D, 0x00, 0x8B, 0xEA, 0x89,
    0x51, 0x28, 0x8B, 0xC2, 0x99, 0xBF, 0x01, 0x00,
};
// iRenderable::GetMatrixUpdateCount as implemented for entity-backed
// renderables: `add rcx,78h; jmp` to the transform update-count getter.
constexpr std::uint32_t kRenderableGetMatrixUpdateCountRva = 0x00446090U;
constexpr std::array<std::uint8_t, 16>
    kRenderableGetMatrixUpdateCountSignature = {
        0x48, 0x83, 0xC1, 0x78, 0xE9, 0xC7, 0x7C, 0xF1,
        0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
// cImGui, the script-driven GUI behind the menus and the inventory. Its
// script pass stores draw commands once per tick in the vector at +0x300
// (0xA0 bytes each, position at +0x08); Draw replays them every frame.
// DoMouse and End queue the cursor at the mouse position held at +0x4E50,
// which SendMousePosition fills from the pixel position the game read.
constexpr std::uint32_t kImGuiDrawRva = 0x0047C550U;
constexpr std::array<std::uint8_t, 32> kImGuiDrawSignature = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00,
    0x00, 0x4C, 0x8B, 0xA1, 0x50, 0x03, 0x00, 0x00,
    0x33, 0xED, 0x4C, 0x2B, 0xA1, 0x48, 0x03, 0x00,
};
constexpr std::uint32_t kImGuiDoMouseRva = 0x004790F0U;
constexpr std::array<std::uint8_t, 32> kImGuiDoMouseSignature = {
    0x48, 0x8B, 0xC4, 0x41, 0x54, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00,
    0x80, 0xB9, 0xA9, 0x06, 0x00, 0x00, 0x00, 0x4D,
    0x8B, 0xF9, 0x4C, 0x8B, 0xE2, 0x4C, 0x8B, 0xF1,
};
constexpr std::uint32_t kImGuiEndRva = 0x0047F440U;
constexpr std::array<std::uint8_t, 32> kImGuiEndSignature = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x57,
    0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0,
    0x00, 0x00, 0x00, 0x80, 0xB9, 0xA9, 0x06, 0x00,
    0x00, 0x00, 0x4C, 0x8B, 0xF9, 0x48, 0x89, 0x58,
};
constexpr std::uint32_t kImGuiSendMousePositionRva = 0x004876D0U;
constexpr std::array<std::uint8_t, 32> kImGuiSendMousePositionSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81, 0x48, 0x4E,
    0x00, 0x00, 0x48, 0x8B, 0xE9, 0x89, 0x81, 0x50,
    0x4E, 0x00, 0x00, 0x49, 0x8B, 0xF0, 0x8B, 0x81,
};
constexpr std::uint32_t kImGuiSendMouseVirtualPositionRva = 0x00487820U;
constexpr std::array<std::uint8_t, 32>
    kImGuiSendMouseVirtualPositionSignature = {
        0x8B, 0x81, 0x48, 0x4E, 0x00, 0x00, 0x89, 0x81,
        0x50, 0x4E, 0x00, 0x00, 0x8B, 0x81, 0x4C, 0x4E,
        0x00, 0x00, 0x89, 0x81, 0x54, 0x4E, 0x00, 0x00,
        0xF3, 0x0F, 0x10, 0x02, 0xF3, 0x0F, 0x11, 0x81,
};
// iParticleEmitter's per-view graphics update (vtable slot +0x68, shared by
// cParticleEmitter_UserData): builds the vertices from the live particle
// list at +0x1E8 (count at +0x200). Each cParticle starts with its position
// and, at +0x0C, the position before the last motion update.
constexpr std::uint32_t kParticleUpdateGraphicsRva = 0x005A6080U;
constexpr std::array<std::uint8_t, 32> kParticleUpdateGraphicsSignature = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,
    0x54, 0x41, 0x56, 0x48, 0x8D, 0xA8, 0x58, 0xFE,
    0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x78, 0x02, 0x00,
    0x00, 0x0F, 0x29, 0x70, 0xB8, 0x48, 0x8B, 0xF1,
};
constexpr std::uint32_t kEntityGetTransformUpdateCountRva = 0x0035DD60U;
constexpr std::array<std::uint8_t, 7>
    kEntityGetTransformUpdateCountSignature = {
        0x8B, 0x81, 0xE0, 0x00, 0x00, 0x00, 0xC3,
};
// cLightSpot::GetViewProjMatrix: returns the cached
// bias * projection * inverse(world) at light+0x408, used for shadow and
// gobo projection. The bias matrix is the constant at RVA 0x0094C070.
constexpr std::uint32_t kLightSpotGetViewProjMatrixRva = 0x004533B0U;
constexpr std::array<std::uint8_t, 32> kLightSpotGetViewProjMatrixSignature = {
    0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
    0xEC, 0x60, 0x48, 0x8B, 0xF9, 0x48, 0x83, 0xC1,
    0x78, 0xE8, 0x9A, 0xA9, 0xF0, 0xFF, 0x39, 0x87,
    0xF4, 0x04, 0x00, 0x00, 0x75, 0x24, 0x80, 0xBF,
};
constexpr std::uint32_t kLightSpotGetProjectionMatrixRva = 0x00453190U;
constexpr std::array<std::uint8_t, 32>
    kLightSpotGetProjectionMatrixSignature = {
        0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0xA0,
        0x00, 0x00, 0x00, 0x80, 0xB9, 0xF0, 0x04, 0x00,
        0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x84, 0x68,
        0x01, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xE8, 0x0F,
};
constexpr std::uint32_t kLightProjectionBiasMatrixRva = 0x0094C070U;
// cLightSpot::GetViewMatrix: cached inverse(world) at light+0x448.
constexpr std::uint32_t kLightSpotGetViewMatrixRva = 0x00453330U;
constexpr std::array<std::uint8_t, 32> kLightSpotGetViewMatrixSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48,
    0x8B, 0xD9, 0x48, 0x83, 0xC1, 0x78, 0xE8, 0x15,
    0xAA, 0xF0, 0xFF, 0x48, 0x8D, 0xBB, 0x48, 0x04,
};
// cLightSpot::GetFrustum: rebuilds the frustum at light+0x4C8 from the view
// and projection matrices when the transform count differs from the tag at
// light+0x4FC. The shadow map is drawn through this frustum.
constexpr std::uint32_t kLightSpotGetFrustumRva = 0x00453030U;
constexpr std::array<std::uint8_t, 32> kLightSpotGetFrustumSignature = {
    0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x81,
    0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1,
    0x48, 0x83, 0xC1, 0x78, 0xE8, 0x17, 0xAD, 0xF0,
    0xFF, 0x39, 0x86, 0xFC, 0x04, 0x00, 0x00, 0x75,
};
// Renderer decision whether a light's cached shadow map must be redrawn:
// (renderer, light, shadowData, casterList, cacheUsable, flag) -> bool.
// Its single call site is at RVA 0x00584F99.
constexpr std::uint32_t kShadowMapNeedsUpdateRva = 0x003583C0U;
constexpr std::array<std::uint8_t, 32> kShadowMapNeedsUpdateSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83,
    0xEC, 0x40, 0x49, 0x8B, 0xF1, 0x4D, 0x85, 0xC0,
    0x0F, 0x84, 0xD5, 0x00, 0x00, 0x00, 0x80, 0x7C,
    0x24, 0x70, 0x00, 0x49, 0x8D, 0x58, 0x18, 0x75,
};
constexpr std::uint32_t kLimitFpsGetterRva = 0x0067F6B0U;
constexpr std::array<std::uint8_t, 8> kLimitFpsGetterSignature = {
    0x0F, 0xB6, 0x81, 0x88, 0x00, 0x00, 0x00, 0xC3,
};
constexpr std::uint32_t kEngineGlobalPointerRva = 0x0096F888U;
constexpr std::uint32_t kSceneGlobalPointerRva = 0x0096F890U;
constexpr std::uint32_t kSceneSetCurrentListenerRva = 0x004253D0U;
constexpr std::array<std::uint8_t, 24> kSceneSetCurrentListenerSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
    0x8B, 0xB9, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B,
};
constexpr std::uint32_t kViewportSetCameraRva = 0x00706D00U;
constexpr std::array<std::uint8_t, 23> kViewportSetCameraSignature = {
    0x48, 0x89, 0x51, 0x18, 0x48, 0x8B, 0xC2, 0x48,
    0x85, 0xD2, 0x74, 0x0B, 0x8B, 0x51, 0x6C, 0x48,
    0x8B, 0xC8, 0xE9, 0x59, 0x09, 0xCC, 0xFF,
};
constexpr std::uint32_t kCameraGetPositionRva = 0x0022C620U;
constexpr std::array<std::uint8_t, 5> kCameraGetPositionSignature = {
    0x48, 0x8D, 0x41, 0x10, 0xC3,
};
constexpr std::uint32_t kCameraSetPitchRva = 0x003C73A0U;
constexpr std::array<std::uint8_t, 16> kCameraSetPitchSignature = {
    0xF3, 0x0F, 0x10, 0x51, 0x54, 0x0F, 0x57, 0xC0,
    0x0F, 0x2E, 0xD0, 0xF3, 0x0F, 0x11, 0x49, 0x48,
};
constexpr std::uint32_t kCameraSetYawRva = 0x003C7620U;
constexpr std::array<std::uint8_t, 16> kCameraSetYawSignature = {
    0xF3, 0x0F, 0x10, 0x51, 0x5C, 0x0F, 0x57, 0xC0,
    0x0F, 0x2E, 0xD0, 0xF3, 0x0F, 0x11, 0x49, 0x4C,
};
constexpr std::uint32_t kCameraSetRollRva = 0x003C7480U;
constexpr std::array<std::uint8_t, 21> kCameraSetRollSignature = {
    0xF3, 0x0F, 0x11, 0x49, 0x50, 0xC6, 0x81, 0xF1,
    0x06, 0x00, 0x00, 0x01, 0x66, 0xC7, 0x81, 0xF3,
    0x06, 0x00, 0x00, 0x01, 0x01,
};
constexpr std::uint32_t kCameraSetPositionRva = 0x003C73F0U;
constexpr std::array<std::uint8_t, 32> kCameraSetPositionSignature = {
    0x8B, 0x02, 0x4C, 0x8D, 0x41, 0x10, 0x41, 0x89,
    0x00, 0x8B, 0x42, 0x04, 0x41, 0x89, 0x40, 0x04,
    0x8B, 0x42, 0x08, 0x49, 0x8B, 0xD0, 0xC6, 0x81,
    0xF1, 0x06, 0x00, 0x00, 0x01, 0x66, 0xC7, 0x81,
};
constexpr std::uint32_t kCameraGetViewMatrixRva = 0x003C5DD0U;
constexpr std::array<std::uint8_t, 32> kCameraGetViewMatrixSignature = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9,
    0xF1, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9,
    0x74, 0x0F, 0xE8, 0xB9, 0x1D, 0x00, 0x00, 0x48,
    0x8D, 0x43, 0x78, 0x48, 0x83, 0xC4, 0x20, 0x5B,
};
constexpr std::uint32_t kCameraGetMoveMatrixRva = 0x003C5C70U;
constexpr std::array<std::uint8_t, 27> kCameraGetMoveMatrixSignature = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0xE8, 0x32, 0x22, 0x00, 0x00, 0x48, 0x8D,
    0x83, 0x38, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x20, 0x5B, 0xC3,
};
constexpr std::uint32_t kCameraGetFrustumRva = 0x003C5AA0U;
constexpr std::array<std::uint8_t, 32> kCameraGetFrustumSignature = {
    0x40, 0x55, 0x48, 0x83, 0xEC, 0x70, 0x80, 0xB9,
    0xF4, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xE9,
    0x0F, 0x84, 0x66, 0x01, 0x00, 0x00, 0x4C, 0x89,
    0xA4, 0x24, 0x98, 0x00, 0x00, 0x00, 0x4C, 0x89,
};
constexpr std::uint32_t kCameraGetExtendedFrustumRva = 0x003C5480U;
constexpr std::array<std::uint8_t, 32> kCameraGetExtendedFrustumSignature = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x58,
    0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x01,
};
constexpr std::uint32_t kSceneRenderAllRva = 0x00424B20U;
constexpr std::array<std::uint8_t, 32> kSceneRenderAllSignature = {
    0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x60,
    0xFF, 0x05, 0x12, 0x77, 0x53, 0x00, 0x48, 0x89,
    0x68, 0x08, 0x33, 0xED, 0x48, 0x89, 0x78, 0x18,
    0x48, 0x8B, 0xF9, 0x0F, 0x29, 0x70, 0xE8, 0x0F,
};
constexpr std::uint32_t kEntitySetMatrixRva = 0x001988E0U;
constexpr std::array<std::uint8_t, 32> kEntitySetMatrixSignature = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
    0x01, 0x48, 0x8B, 0xD9, 0xC6, 0x81, 0xCB, 0x03,
    0x00, 0x00, 0x01, 0xFF, 0x90, 0x70, 0x01, 0x00,
    0x00, 0x48, 0x8B, 0xCB, 0xE8, 0x7F, 0x51, 0x00,
};
constexpr std::uint32_t kEntityUpdateAttachmentRva = 0x0019BE50U;
constexpr std::array<std::uint8_t, 32> kEntityUpdateAttachmentSignature = {
    0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xE8,
    0xFC, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x18, 0x04,
    0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89,
    0x90, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F,
};
constexpr std::uint32_t kSubMeshGetModelMatrixRva = 0x004460A0U;
constexpr std::array<std::uint8_t, 16> kSubMeshGetModelMatrixSignature = {
    0x48, 0x83, 0xC1, 0x78, 0xE9, 0xC7, 0x7C, 0xF1,
    0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::uint32_t kSubMeshGetSkinningPaletteRva = 0x004ADAC0U;
constexpr std::array<std::uint8_t, 20> kSubMeshGetSkinningPaletteSignature = {
    0x48, 0x8B, 0x81, 0xB0, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC0,
    0x74, 0x08, 0x48, 0x8B, 0x80, 0x18, 0x03, 0x00, 0x00, 0xC3,
};
constexpr std::uint32_t kSubMeshUpdateSkinningRva = 0x004ADEB0U;
constexpr std::array<std::uint8_t, 32> kSubMeshUpdateSkinningSignature = {
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xB0,
    0x01, 0x00, 0x00, 0xE8, 0x38, 0x65, 0xE8, 0xFF,
    0x48, 0x83, 0xBB, 0xD8, 0x01, 0x00, 0x00, 0x00,
};
constexpr std::uint32_t kMeshUpdatePaletteRva = 0x00334400U;
constexpr std::array<std::uint8_t, 32> kMeshUpdatePaletteSignature = {
    0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x08, 0x01,
    0x00, 0x00, 0x80, 0xB9, 0xB0, 0x01, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xF1, 0x75, 0x11, 0xE8, 0x45,
    0x99, 0x02, 0x00, 0x39, 0x86, 0xB4, 0x01, 0x00,
};

bool RangeContains(const std::uint32_t rangeRva,
                   const std::uint32_t rangeSize,
                   const std::uint32_t itemRva,
                   const std::size_t itemSize) noexcept {
    if (itemSize > UINT32_MAX) {
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(itemSize);
    return itemRva >= rangeRva && itemRva - rangeRva <= rangeSize &&
           size <= rangeSize - (itemRva - rangeRva);
}

template <std::size_t Size>
bool ValidateSignature(const target::HostImage& image,
                       const std::uint32_t rva,
                       const std::array<std::uint8_t, Size>& expected) noexcept {
    if (!RangeContains(image.textRva, image.textSize, rva, expected.size()) ||
        image.imageBase == 0) {
        return false;
    }
    const auto* const address =
        reinterpret_cast<const void*>(image.imageBase + rva);
    return std::memcmp(address, expected.data(), expected.size()) == 0;
}

} // namespace

bool ResolveTelemetryMap(const target::HostImage& image,
                         const target::KnownBuild& build,
                         TelemetryMap& map,
                         std::wstring& error) noexcept {
    map = {};
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No telemetry mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kSteadySwapWrapperRva,
                           kSteadySwapWrapperSignature)) {
        error = L"Steady swap wrapper signature mismatch at RVA 0x005EC4F0";
        return false;
    }
    if (!ValidateSignature(image, kLogicTimerWantUpdateRva,
                           kLogicTimerWantUpdateSignature)) {
        error = L"Logic timer WantUpdate signature mismatch at RVA 0x00555600";
        return false;
    }
    if (!ValidateSignature(image, kUpdaterDispatchRva,
                           kUpdaterDispatchSignature)) {
        error = L"Updater dispatch signature mismatch at RVA 0x0039E900";
        return false;
    }
    if (!ValidateSignature(image, kEngineMicrosecondClockRva,
                           kEngineMicrosecondClockSignature)) {
        error = L"Engine microsecond clock signature mismatch at RVA 0x00395FE0";
        return false;
    }
    if (!ValidateSignature(image, kLimitFpsGetterRva,
                           kLimitFpsGetterSignature)) {
        error = L"LimitFPS getter signature mismatch at RVA 0x0067F6B0";
        return false;
    }
    if (!RangeContains(0, image.imageSize, kEngineGlobalPointerRva,
                       sizeof(std::uintptr_t))) {
        error = L"Engine global pointer is outside the mapped image";
        return false;
    }
    map.steadySwapWrapper = image.imageBase + kSteadySwapWrapperRva;
    map.logicTimerWantUpdate = image.imageBase + kLogicTimerWantUpdateRva;
    map.updaterDispatch = image.imageBase + kUpdaterDispatchRva;
    map.engineMicrosecondClock = image.imageBase + kEngineMicrosecondClockRva;
    map.limitFpsGetter = image.imageBase + kLimitFpsGetterRva;
    map.engineGlobalPointer = image.imageBase + kEngineGlobalPointerRva;
    return true;
}

bool ResolveWorldMap(const target::HostImage& image,
                     const target::KnownBuild& build,
                     TelemetryMap& map,
                     std::wstring& error) noexcept {
    map.renderableGetMatrixUpdateCount = 0;
    map.lightSpotGetViewProjMatrix = 0;
    map.lightSpotGetProjectionMatrix = 0;
    map.lightProjectionBiasMatrix = 0;
    map.shadowMapNeedsUpdate = 0;
    map.lightSpotGetViewMatrix = 0;
    map.lightSpotGetFrustum = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No world-object mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kRenderableGetMatrixUpdateCountRva,
                           kRenderableGetMatrixUpdateCountSignature) ||
        !ValidateSignature(image, kEntityGetTransformUpdateCountRva,
                           kEntityGetTransformUpdateCountSignature)) {
        error = L"Renderable matrix update-count signature mismatch at RVA "
                L"0x00446090";
        return false;
    }
    if (!ValidateSignature(image, kLightSpotGetViewProjMatrixRva,
                           kLightSpotGetViewProjMatrixSignature) ||
        !ValidateSignature(image, kLightSpotGetProjectionMatrixRva,
                           kLightSpotGetProjectionMatrixSignature) ||
        !ValidateSignature(image, kShadowMapNeedsUpdateRva,
                           kShadowMapNeedsUpdateSignature) ||
        !ValidateSignature(image, kLightSpotGetViewMatrixRva,
                           kLightSpotGetViewMatrixSignature) ||
        !ValidateSignature(image, kLightSpotGetFrustumRva,
                           kLightSpotGetFrustumSignature) ||
        !RangeContains(0, image.imageSize, kLightProjectionBiasMatrixRva,
                       16 * sizeof(float))) {
        error = L"Spot-light projection signature mismatch at RVA "
                L"0x004533B0";
        return false;
    }
    map.renderableGetMatrixUpdateCount =
        image.imageBase + kRenderableGetMatrixUpdateCountRva;
    map.lightSpotGetViewProjMatrix =
        image.imageBase + kLightSpotGetViewProjMatrixRva;
    map.lightSpotGetProjectionMatrix =
        image.imageBase + kLightSpotGetProjectionMatrixRva;
    map.lightProjectionBiasMatrix =
        image.imageBase + kLightProjectionBiasMatrixRva;
    map.shadowMapNeedsUpdate = image.imageBase + kShadowMapNeedsUpdateRva;
    map.lightSpotGetViewMatrix = image.imageBase + kLightSpotGetViewMatrixRva;
    map.lightSpotGetFrustum = image.imageBase + kLightSpotGetFrustumRva;
    return true;
}

bool ResolveGuiMap(const target::HostImage& image,
                   const target::KnownBuild& build,
                   TelemetryMap& map,
                   std::wstring& error) noexcept {
    map.imGuiDraw = 0;
    map.imGuiDoMouse = 0;
    map.imGuiEnd = 0;
    map.imGuiSendMousePosition = 0;
    map.imGuiSendMouseVirtualPosition = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No GUI mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kImGuiDrawRva, kImGuiDrawSignature) ||
        !ValidateSignature(image, kImGuiDoMouseRva, kImGuiDoMouseSignature) ||
        !ValidateSignature(image, kImGuiEndRva, kImGuiEndSignature) ||
        !ValidateSignature(image, kImGuiSendMousePositionRva,
                           kImGuiSendMousePositionSignature) ||
        !ValidateSignature(image, kImGuiSendMouseVirtualPositionRva,
                           kImGuiSendMouseVirtualPositionSignature)) {
        error = L"cImGui signature mismatch at RVA 0x0047C550";
        return false;
    }
    map.imGuiDraw = image.imageBase + kImGuiDrawRva;
    map.imGuiDoMouse = image.imageBase + kImGuiDoMouseRva;
    map.imGuiEnd = image.imageBase + kImGuiEndRva;
    map.imGuiSendMousePosition = image.imageBase + kImGuiSendMousePositionRva;
    map.imGuiSendMouseVirtualPosition =
        image.imageBase + kImGuiSendMouseVirtualPositionRva;
    return true;
}

bool ResolveParticleMap(const target::HostImage& image,
                        const target::KnownBuild& build,
                        TelemetryMap& map,
                        std::wstring& error) noexcept {
    map.particleUpdateGraphics = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No particle mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kParticleUpdateGraphicsRva,
                           kParticleUpdateGraphicsSignature)) {
        error = L"Particle graphics-update signature mismatch at RVA "
                L"0x005A6080";
        return false;
    }
    map.particleUpdateGraphics = image.imageBase + kParticleUpdateGraphicsRva;
    return true;
}

bool ResolveUpdateRateMap(const target::HostImage& image,
                          const target::KnownBuild& build,
                          TelemetryMap& map,
                          std::wstring& error) noexcept {
    map.engineSetUpdatesPerSec = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No update-rate mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kEngineSetUpdatesPerSecRva,
                           kEngineSetUpdatesPerSecSignature)) {
        error = L"cEngine::SetUpdatesPerSec signature mismatch at RVA "
                L"0x0039DAC0";
        return false;
    }
    if (!ValidateSignature(image, kLogicTimerSetUpdatesPerSecRva,
                           kLogicTimerSetUpdatesPerSecSignature)) {
        error = L"Logic timer SetUpdatesPerSec signature mismatch at RVA "
                L"0x00555530";
        return false;
    }
    map.engineSetUpdatesPerSec = image.imageBase + kEngineSetUpdatesPerSecRva;
    return true;
}

bool ResolveCameraTraceMap(const target::HostImage& image,
                           const target::KnownBuild& build,
                           TelemetryMap& map,
                           std::wstring& error) noexcept {
    map.sceneGlobalPointer = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No camera-trace mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kSceneSetCurrentListenerRva,
                           kSceneSetCurrentListenerSignature)) {
        error = L"Scene current-listener signature mismatch at RVA 0x004253D0";
        return false;
    }
    if (!ValidateSignature(image, kViewportSetCameraRva,
                           kViewportSetCameraSignature)) {
        error = L"Viewport camera signature mismatch at RVA 0x00706D00";
        return false;
    }
    if (!ValidateSignature(image, kCameraGetPositionRva,
                           kCameraGetPositionSignature)) {
        error = L"Camera position signature mismatch at RVA 0x0022C620";
        return false;
    }
    if (!ValidateSignature(image, kCameraSetPitchRva,
                           kCameraSetPitchSignature) ||
        !ValidateSignature(image, kCameraSetYawRva,
                           kCameraSetYawSignature) ||
        !ValidateSignature(image, kCameraSetRollRva,
                           kCameraSetRollSignature)) {
        error = L"Camera orientation layout signature mismatch";
        return false;
    }
    if (!RangeContains(0, image.imageSize, kSceneGlobalPointerRva,
                       sizeof(std::uintptr_t))) {
        error = L"Scene global pointer is outside the mapped image";
        return false;
    }
    map.sceneGlobalPointer = image.imageBase + kSceneGlobalPointerRva;
    return true;
}

bool ResolveCameraRuntimeMap(const target::HostImage& image,
                             const target::KnownBuild& build,
                             TelemetryMap& map,
                             std::wstring& error) noexcept {
    map.sceneRenderAll = 0;
    map.cameraGetViewMatrix = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No camera-runtime mapping exists for this executable build";
        return false;
    }
    if (map.sceneGlobalPointer == 0 &&
        !ResolveCameraTraceMap(image, build, map, error)) {
        return false;
    }
    if (!ValidateSignature(image, kCameraSetPositionRva,
                           kCameraSetPositionSignature)) {
        error = L"Camera position/dirty-cache signature mismatch at RVA 0x003C73F0";
        return false;
    }
    if (!ValidateSignature(image, kCameraGetViewMatrixRva,
                           kCameraGetViewMatrixSignature) ||
        !ValidateSignature(image, kCameraGetMoveMatrixRva,
                           kCameraGetMoveMatrixSignature)) {
        error = L"Camera matrix-cache signature mismatch";
        return false;
    }
    if (!ValidateSignature(image, kCameraGetFrustumRva,
                           kCameraGetFrustumSignature) ||
        !ValidateSignature(image, kCameraGetExtendedFrustumRva,
                           kCameraGetExtendedFrustumSignature)) {
        error = L"Camera frustum-cache signature mismatch";
        return false;
    }
    if (!ValidateSignature(image, kSceneRenderAllRva,
                           kSceneRenderAllSignature)) {
        error = L"Scene RenderAll signature mismatch at RVA 0x00424B20";
        return false;
    }
    map.sceneRenderAll = image.imageBase + kSceneRenderAllRva;
    map.cameraGetViewMatrix = image.imageBase + kCameraGetViewMatrixRva;
    return true;
}

bool ResolveHandsTraceMap(const target::HostImage& image,
                          const target::KnownBuild& build,
                          TelemetryMap& map,
                          std::wstring& error) noexcept {
    map.entitySetMatrix = 0;
    map.entityUpdateAttachment = 0;
    map.subMeshGetModelMatrix = 0;
    map.subMeshGetSkinningPalette = 0;
    map.subMeshUpdateSkinning = 0;
    map.meshUpdatePalette = 0;
    error.clear();
    if (build.id != target::BuildId::gog_57301760292462330) {
        error = L"No hands-trace mapping exists for this executable build";
        return false;
    }
    if (!ValidateSignature(image, kEntitySetMatrixRva,
                           kEntitySetMatrixSignature)) {
        error = L"iLuxEntity::SetMatrix signature mismatch at RVA 0x001988E0";
        return false;
    }
    if (!ValidateSignature(image, kEntityUpdateAttachmentRva,
                           kEntityUpdateAttachmentSignature)) {
        error = L"iLuxEntity::UpdateEntityAttachment signature mismatch at "
                L"RVA 0x0019BE50";
        return false;
    }
    if (!ValidateSignature(image, kSubMeshGetModelMatrixRva,
                           kSubMeshGetModelMatrixSignature)) {
        error = L"cSubMeshEntity::GetModelMatrix signature mismatch at "
                L"RVA 0x004460A0";
        return false;
    }
    if (!ValidateSignature(image, kSubMeshGetSkinningPaletteRva,
                           kSubMeshGetSkinningPaletteSignature)) {
        error = L"cSubMeshEntity skinning-palette getter signature mismatch at "
                L"RVA 0x004ADAC0";
        return false;
    }
    if (!ValidateSignature(image, kSubMeshUpdateSkinningRva,
                           kSubMeshUpdateSkinningSignature)) {
        error = L"cSubMeshEntity skinning signature mismatch at RVA 0x004ADEB0";
        return false;
    }
    if (!ValidateSignature(image, kMeshUpdatePaletteRva,
                           kMeshUpdatePaletteSignature)) {
        error = L"cMeshEntity palette-update signature mismatch at "
                L"RVA 0x00334400";
        return false;
    }
    map.entitySetMatrix = image.imageBase + kEntitySetMatrixRva;
    map.entityUpdateAttachment = image.imageBase + kEntityUpdateAttachmentRva;
    map.subMeshGetModelMatrix = image.imageBase + kSubMeshGetModelMatrixRva;
    map.subMeshGetSkinningPalette =
        image.imageBase + kSubMeshGetSkinningPaletteRva;
    map.subMeshUpdateSkinning = image.imageBase + kSubMeshUpdateSkinningRva;
    map.meshUpdatePalette = image.imageBase + kMeshUpdatePaletteRva;
    return true;
}

} // namespace atb::mapping
