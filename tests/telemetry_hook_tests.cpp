#include "runtime_mapping.h"
#include "telemetry.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

int main(const int argumentCount, char** arguments) {
    const bool renderOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--render-unlock-only";
    const bool limiterOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--presentation-limiter-only";
    const bool cameraTraceOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--camera-trace-only";
    const bool cameraRestoreOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--camera-restore-probe-only";
    const bool handsTraceOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--hands-trace-only";
    const bool handsPoseRestoreOnly = argumentCount == 2 &&
        std::string_view(arguments[1]) == "--hands-pose-restore-only";
    const bool handsTraceMode = handsTraceOnly || handsPoseRestoreOnly;
    if (argumentCount != 1 && !renderOnly && !limiterOnly && !cameraTraceOnly &&
        !cameraRestoreOnly && !handsTraceOnly && !handsPoseRestoreOnly) {
        std::cerr << "usage: telemetry_hook_tests "
                     "[--render-unlock-only|--presentation-limiter-only|"
                     "--camera-trace-only|--camera-restore-probe-only|"
                     "--hands-trace-only|--hands-pose-restore-only]\n";
        return EXIT_FAILURE;
    }
    constexpr std::size_t allocationSize = 0x1000;
    auto* const code = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, allocationSize, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (code == nullptr) {
        std::cerr << "FAIL: VirtualAlloc returned " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    // Same instruction shape as the frozen Bunker wrapper:
    //   mov rcx, [rcx+30h]
    //   jmp rel32
    // The local destination is a plain return, which is sufficient to prove
    // that MinHook relocates the short tail-call wrapper successfully.
    constexpr std::array<std::uint8_t, 9> wrapper = {
        0x48, 0x8B, 0x49, 0x30, 0xE9, 0x17, 0x00, 0x00, 0x00,
    };
    std::memcpy(code, wrapper.data(), wrapper.size());
    code[0x20] = 0xC3;
    // Return maximumUpdates != 0, allowing both accepted and rejected polling
    // paths to be checked without depending on compiled helper code.
    constexpr std::array<std::uint8_t, 8> wantUpdate = {
        0x31, 0xC0,             // xor eax, eax
        0x85, 0xD2,             // test edx, edx
        0x0F, 0x95, 0xC0,       // setne al
        0xC3,                   // ret
    };
    std::memcpy(code + 0x40, wantUpdate.data(), wantUpdate.size());
    // Relocatable synthetic updater-dispatch function.
    std::memset(code + 0x80, 0x90, 16);
    code[0x90] = 0xC3;
    // mov rax, 995833; ret -- a fixed engine-clock sample halfway through
    // an 8333.333 us scheduler interval ending at 1,000,000 us.
    code[0xC0] = 0x48;
    code[0xC1] = 0xB8;
    const std::int64_t syntheticNow = 995'833;
    std::memcpy(code + 0xC2, &syntheticNow, sizeof(syntheticNow));
    code[0xCA] = 0xC3;
    // Relocatable synthetic five-argument scene RenderAll function.
    std::memset(code + 0x100, 0x90, 16);
    code[0x110] = 0xC3;
    // Relocatable synthetic entity SetMatrix and attachment functions.
    std::memset(code + 0x140, 0x90, 16);
    code[0x150] = 0xC3;
    std::memset(code + 0x180, 0x90, 16);
    code[0x190] = 0xC3;
    // Relocatable pass-through model-matrix target: mov rax, matrix; ret.
    alignas(float) std::array<float, 16> syntheticModelMatrix{};
    syntheticModelMatrix[15] = 1.0F;
    code[0x1C0] = 0x48;
    code[0x1C1] = 0xB8;
    const auto syntheticModelMatrixAddress =
        reinterpret_cast<std::uintptr_t>(syntheticModelMatrix.data());
    std::memcpy(code + 0x1C2, &syntheticModelMatrixAddress,
                sizeof(syntheticModelMatrixAddress));
    code[0x1CA] = 0xC3;
    // Relocatable pass-through synthetic skinned-submesh update.
    std::memset(code + 0x200, 0x90, 16);
    code[0x210] = 0xC3;
    // Relocatable pass-through synthetic mesh palette update.
    std::memset(code + 0x240, 0x90, 16);
    code[0x250] = 0xC3;
    // Relocatable pass-through renderer palette getter.
    alignas(float) std::array<float, 8> syntheticSkinningPalette{};
    syntheticSkinningPalette[3] = 1.0F;
    code[0x280] = 0x48;
    code[0x281] = 0xB8;
    const auto syntheticSkinningPaletteAddress =
        reinterpret_cast<std::uintptr_t>(syntheticSkinningPalette.data());
    std::memcpy(code + 0x282, &syntheticSkinningPaletteAddress,
                sizeof(syntheticSkinningPaletteAddress));
    code[0x28A] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), code, allocationSize);

    atb::mapping::TelemetryMap map{};
    map.steadySwapWrapper = reinterpret_cast<std::uintptr_t>(code);
    map.logicTimerWantUpdate = reinterpret_cast<std::uintptr_t>(code + 0x40);
    map.updaterDispatch = reinterpret_cast<std::uintptr_t>(code + 0x80);
    map.engineMicrosecondClock = reinterpret_cast<std::uintptr_t>(code + 0xC0);
    map.limitFpsGetter = reinterpret_cast<std::uintptr_t>(code + 0xE0);
    map.sceneRenderAll = reinterpret_cast<std::uintptr_t>(code + 0x100);
    map.entitySetMatrix = reinterpret_cast<std::uintptr_t>(code + 0x140);
    map.entityUpdateAttachment = reinterpret_cast<std::uintptr_t>(code + 0x180);
    map.subMeshGetModelMatrix =
        reinterpret_cast<std::uintptr_t>(code + 0x1C0);
    map.subMeshGetSkinningPalette =
        reinterpret_cast<std::uintptr_t>(code + 0x280);
    map.subMeshUpdateSkinning =
        reinterpret_cast<std::uintptr_t>(code + 0x200);
    map.meshUpdatePalette = reinterpret_cast<std::uintptr_t>(code + 0x240);
    std::array<std::uint8_t, 0x89> engine{};
    engine[0x88] = 1;
    void* enginePointer = engine.data();
    map.engineGlobalPointer = reinterpret_cast<std::uintptr_t>(&enginePointer);
    alignas(void*) std::array<std::uint8_t, 0x100> scene{};
    alignas(void*) std::array<std::uint8_t, 0x100> listener{};
    alignas(void*) std::array<std::uint8_t, 0x740> camera{};
    alignas(void*) std::array<std::uint8_t, 0x40> world{};
    alignas(void*) std::array<std::uint8_t, 0x308> extendedFrustum{};
    void* scenePointer = scene.data();
    void* listenerPointer = listener.data();
    void* cameraPointer = camera.data();
    void* worldPointer = world.data();
    void* extendedFrustumPointer = extendedFrustum.data();
    std::memcpy(scene.data() + 0x90, &listenerPointer, sizeof(listenerPointer));
    std::memcpy(listener.data() + 0x18, &cameraPointer, sizeof(cameraPointer));
    std::memcpy(listener.data() + 0x20, &worldPointer, sizeof(worldPointer));
    listener[0x29] = 1;
    std::memcpy(camera.data() + 0x6E8, &extendedFrustumPointer,
                sizeof(extendedFrustumPointer));
    map.sceneGlobalPointer = reinterpret_cast<std::uintptr_t>(&scenePointer);
    const atb::telemetry::Options options{
        !renderOnly && !limiterOnly,
        cameraTraceOnly || cameraRestoreOnly || handsTraceMode,
        cameraRestoreOnly,
        false,
        handsTraceMode,
        false,
        handsPoseRestoreOnly,
        false,
        false,
        false,
        false,
        renderOnly || limiterOnly,
        limiterOnly,
        limiterOnly ? 1000U : 0U,
        60,
    };
    std::wstring error;
    if (!atb::telemetry::Install(map, options, error)) {
        std::wcerr << L"FAIL: telemetry hook rejected synthetic wrapper: "
                   << error << L'\n';
        return EXIT_FAILURE;
    }

    alignas(void*) std::array<std::uint8_t, 0x38> graphics{};
    using Wrapper = void (*)(void*);
    const auto invoke = reinterpret_cast<Wrapper>(code);
    using WantUpdate = std::uint8_t (*)(void*, int);
    const auto wantUpdateFunction =
        reinterpret_cast<WantUpdate>(code + 0x40);
    alignas(void*) std::array<std::uint8_t, 0x40> timer{};
    const std::uint32_t configuredRate = 120;
    const std::int64_t nextTick = 1'000'000;
    const double stepMicroseconds = 1'000'000.0 / 120.0;
    const double speedMultiplier = 1.0;
    std::memcpy(timer.data(), &nextTick, sizeof(nextTick));
    std::memcpy(timer.data() + 0x08, &stepMicroseconds,
                sizeof(stepMicroseconds));
    std::memcpy(timer.data() + 0x30, &speedMultiplier,
                sizeof(speedMultiplier));
    std::memcpy(timer.data() + 0x28, &configuredRate, sizeof(configuredRate));
    if (!renderOnly) {
        if (wantUpdateFunction(timer.data(), 4) == 0) {
            std::cerr << "FAIL: hooked synthetic logic timer changed its result\n";
            return EXIT_FAILURE;
        }
        if (wantUpdateFunction(timer.data(), 0) != 0) {
            std::cerr << "FAIL: hooked rejected logic-timer poll changed its result\n";
            return EXIT_FAILURE;
        }
        using UpdaterDispatch = void (*)(void*, int, float);
        const auto updaterDispatch =
            reinterpret_cast<UpdaterDispatch>(code + 0x80);
        updaterDispatch(nullptr, 5, 1.0F / 120.0F);
    }
    if (cameraRestoreOnly) {
        using SceneRenderAll = void (*)(void*, float, std::uint32_t, float,
                                        std::uint32_t);
        const auto sceneRenderAll =
            reinterpret_cast<SceneRenderAll>(code + 0x100);
        sceneRenderAll(scene.data(), 1.0F / 240.0F, 0, 0.0F, 0xFFFFFFFFU);
    }
    if (handsTraceMode) {
        using GetModelMatrix = void* (*)(void*, void*);
        const auto getModelMatrix =
            reinterpret_cast<GetModelMatrix>(code + 0x1C0);
        alignas(void*) std::array<std::uint8_t, 0x220> subMesh{};
        if (getModelMatrix(subMesh.data(), nullptr) !=
            syntheticModelMatrix.data()) {
            std::cerr << "FAIL: pass-through model hook changed matrix pointer\n";
            return EXIT_FAILURE;
        }
        using GetSkinningPalette = const void* (*)(void*);
        const auto getSkinningPalette =
            reinterpret_cast<GetSkinningPalette>(code + 0x280);
        if (getSkinningPalette(subMesh.data()) !=
            syntheticSkinningPalette.data()) {
            std::cerr << "FAIL: pass-through palette hook changed pointer\n";
            return EXIT_FAILURE;
        }
        using UpdateSkinning = void (*)(void*);
        const auto updateSkinning =
            reinterpret_cast<UpdateSkinning>(code + 0x200);
        updateSkinning(subMesh.data());
        if (handsPoseRestoreOnly) {
            const auto updatePalette =
                reinterpret_cast<UpdateSkinning>(code + 0x240);
            updatePalette(nullptr);
        }
    }
    invoke(graphics.data());
    if ((renderOnly || limiterOnly) && engine[0x88] != 0) {
        std::cerr << "FAIL: RenderUnlock did not clear the synthetic engine flag\n";
        return EXIT_FAILURE;
    }
    if (!renderOnly && !limiterOnly && engine[0x88] == 0) {
        std::cerr << "FAIL: telemetry-only mode modified the engine limit flag\n";
        return EXIT_FAILURE;
    }
    invoke(graphics.data());

    // The hook remains installed until process exit, matching ASI lifetime.
    std::cout << (handsPoseRestoreOnly ? "hands-pose restore hooks passed\n" :
                  handsTraceOnly ? "hands-trace hooks passed\n" :
                  cameraRestoreOnly ? "camera-restore probe hook passed\n" :
                  cameraTraceOnly ? "camera-trace hook passed\n" :
                  limiterOnly ? "presentation-limiter-only hook passed\n" :
                  renderOnly ? "render-unlock-only hook passed\n" :
                               "telemetry-only hooks passed\n");
    return EXIT_SUCCESS;
}
