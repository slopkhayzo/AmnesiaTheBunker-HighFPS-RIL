#include "telemetry.h"

#include "camera_runtime.h"
#include "camera_trace.h"
#include "frame_cadence.h"
#include "frame_pacing.h"
#include "frame_trace.h"
#include "guarded_memory.h"
#include "gui_cursor.h"
#include "hands_trace.h"
#include "hands_pose_interpolation.h"
#include "logger.h"
#include "mouse_trace.h"
#include "particle_runtime.h"
#include "presentation_clock.h"
#include "presentation_limiter.h"
#include "world_runtime.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace atb::telemetry {
namespace {

using SwapWrapper = void (*)(void* graphics);
using LogicTimerWantUpdate = std::uint8_t (*)(void* timer, int maximumUpdates);
using UpdaterDispatch = void (*)(void* updater, int message, float stepSeconds);
using EngineMicrosecondClock = std::int64_t (*)();
using EngineSetUpdatesPerSec = void (*)(void* engine, int updatesPerSec,
                                        int catchUpDivisor);
using SceneRenderAll = void (*)(void* scene, float frameTime,
                                std::uint32_t renderFlags, float renderTime,
                                std::uint32_t viewportMask);
using EntitySetMatrix = void (*)(void* entity,
                                 const hands_trace::Matrix* matrix);
using EntityUpdateAttachment = void (*)(void* entity);
using SubMeshGetModelMatrix = hands_trace::Matrix* (*)(void* subMesh,
                                                        void* frustum);
using SubMeshGetSkinningPalette = const void* (*)(void* subMesh);
using SubMeshUpdateSkinning = void (*)(void* subMesh);
using MeshUpdatePalette = void (*)(void* mesh);
using RenderableGetMatrixUpdateCount = int (*)(void* renderable);
using LightSpotGetMatrix = hands_trace::Matrix* (*)(void* light);
using ShadowMapNeedsUpdate = std::uint8_t (*)(void* renderer, void* light,
                                              void* shadowData,
                                              void* casters,
                                              std::uint8_t cacheUsable,
                                              std::uint8_t flag);

SwapWrapper g_originalSwap = nullptr;
LogicTimerWantUpdate g_originalWantUpdate = nullptr;
UpdaterDispatch g_originalUpdaterDispatch = nullptr;
EngineMicrosecondClock g_engineMicrosecondClock = nullptr;
EngineSetUpdatesPerSec g_engineSetUpdatesPerSec = nullptr;
int g_gameplayRateOverride = 0;
std::atomic<std::uint64_t> g_gameplayRateApplications{0};
SceneRenderAll g_originalSceneRenderAll = nullptr;
EntitySetMatrix g_originalEntitySetMatrix = nullptr;
EntityUpdateAttachment g_originalEntityUpdateAttachment = nullptr;
SubMeshGetModelMatrix g_originalSubMeshGetModelMatrix = nullptr;
SubMeshGetSkinningPalette g_originalSubMeshGetSkinningPalette = nullptr;
SubMeshUpdateSkinning g_originalSubMeshUpdateSkinning = nullptr;
MeshUpdatePalette g_originalMeshUpdatePalette = nullptr;
RenderableGetMatrixUpdateCount g_originalRenderableGetMatrixUpdateCount =
    nullptr;
LightSpotGetMatrix g_originalLightSpotGetViewProjMatrix = nullptr;
LightSpotGetMatrix g_lightSpotGetProjectionMatrix = nullptr;
const hands_trace::Matrix* g_lightProjectionBias = nullptr;
std::atomic<std::uint64_t> g_lightProjectionApplied{0};
std::atomic<std::uint64_t> g_lightProjectionRejected{0};
std::atomic<std::uint64_t> g_lightProjectionOverflows{0};
ShadowMapNeedsUpdate g_originalShadowMapNeedsUpdate = nullptr;
using LightSpotGetFrustum = void* (*)(void* light);
LightSpotGetMatrix g_originalLightSpotGetViewMatrix = nullptr;
LightSpotGetFrustum g_originalLightSpotGetFrustum = nullptr;
std::atomic<std::uint64_t> g_lightViewApplied{0};
std::atomic<std::uint64_t> g_lightViewRejected{0};
std::atomic<std::uint64_t> g_lightFrustumRebuilds{0};
// Set only while the engine rebuilds a spot-light frustum for an
// interpolated light; every other caller gets the authoritative view.
thread_local bool t_lightInterpolatedViewScope = false;
std::atomic<std::uint64_t> g_lightShadowChecks{0};
std::atomic<std::uint64_t> g_lightShadowForced{0};
cadence::FrameCadence g_presentCadence;
std::int64_t g_qpcFrequency = 0;
std::int64_t g_reportIntervalTicks = 0;
std::atomic<std::int64_t> g_lastReportCounter{0};
std::atomic<std::uint64_t> g_fixedUpdates{0};
std::atomic<std::uint64_t> g_completedFixedUpdates{0};
std::atomic<std::uint64_t> g_variableUpdates{0};
std::atomic<std::uint64_t> g_drawFrames{0};
std::atomic<std::uint64_t> g_endFrames{0};
std::atomic<std::uint64_t> g_fixedSerial{0};
std::atomic<std::uintptr_t> g_logicTimerPointer{0};
std::atomic<std::uint64_t> g_phaseSamples{0};
std::atomic<std::uint64_t> g_invalidPhaseSamples{0};
std::atomic<std::uint64_t> g_phaseRegressions{0};
std::atomic<std::uint64_t> g_phaseHolds{0};
std::atomic<std::uint32_t> g_latestAlphaPartsPerMillion{0};
std::atomic<std::uint64_t> g_nextFieldUnderRange{0};
std::atomic<std::uint64_t> g_nextFieldOverRange{0};
std::atomic<std::uint64_t> g_completedFieldValid{0};
std::atomic<std::uint64_t> g_completedFieldUnderRange{0};
std::atomic<std::uint64_t> g_completedFieldOverRange{0};
std::atomic<DWORD> g_telemetryThread{0};
std::atomic<std::uint64_t> g_threadMismatches{0};
std::atomic<std::uint32_t> g_configuredUpdateRate{0};
std::atomic<std::uint32_t> g_latestUpdatesThisFrame{0};
std::uintptr_t g_engineGlobalPointer = 0;
std::uintptr_t g_sceneGlobalPointer = 0;
bool g_telemetryEnabled = false;
bool g_motionTraceEnabled = false;
bool g_cameraRestoreProbe = false;
bool g_cameraInterpolation = false;
bool g_handsTraceEnabled = false;
bool g_handsRenderCopyProbe = false;
bool g_handsPoseRestoreProbe = false;
bool g_handsPoseRenderCopyProbe = false;
bool g_handsRootInterpolation = false;
bool g_handsPoseInterpolation = false;
bool g_handsRestoreProbe = false;
bool g_worldTraceEnabled = false;
bool g_mouseTraceEnabled = false;
bool g_guiCursor = false;
bool g_particleInterpolation = false;
bool g_mousePreview = false;
using CameraGetViewMatrix = const float* (*)(void* camera);
CameraGetViewMatrix g_cameraGetViewMatrix = nullptr;
// Set for the duration of one scene render when the camera leads its
// interpolated pose; read by the first-person model-matrix path.
world::LeadTransform g_viewLead{};
bool g_viewLeadActive = false;
std::atomic<std::uint64_t> g_viewLeadFrames{0};
bool g_worldRootInterpolation = false;
bool g_renderUnlock = false;
bool g_presentationLimiter = false;
std::atomic<bool> g_renderUnlockLogged{false};
std::atomic<bool> g_waitFailureLogged{false};
std::atomic<std::uint64_t> g_limiterWaits{0};
std::atomic<std::uint64_t> g_limiterPhaseResets{0};
limiter::DeadlineSchedule g_deadlineSchedule;
// Even frame pacing; main thread only.
bool g_evenFramePacing = false;
pacing::Pacer g_evenPacer;
pacing::Plan g_pacePlan{};
bool g_pacePlanned = false;
std::int64_t g_paceFrameStart = 0;  // the previous frame entered its swap
std::int64_t g_paceWaited = 0;      // this frame's wait before the render
std::int64_t g_paceRenderStart = 0; // this frame's render began
unsigned int g_paceFrameTicks = 0;
bool g_paceTickWasDue = false;
std::atomic<std::uint64_t> g_pacedFrames{0};
std::atomic<std::uint64_t> g_paceLateFrames{0};
std::atomic<std::uint64_t> g_paceFallbackFrames{0};
std::atomic<std::uint64_t> g_paceTickMisses{0};
HANDLE g_waitableTimer = nullptr;
std::int64_t g_spinThresholdTicks = 0;
bool g_highResolutionTimer = false;

std::atomic<std::uint64_t> g_cameraCompletedSamples{0};
std::atomic<std::uint64_t> g_cameraCompletedChanges{0};
std::atomic<std::uint64_t> g_cameraCompletedStable{0};
std::atomic<std::uint64_t> g_cameraVariableSamples{0};
std::atomic<std::uint64_t> g_cameraVariableChanges{0};
std::atomic<std::uint64_t> g_cameraVariableStable{0};
std::atomic<std::uint64_t> g_cameraDrawSamples{0};
std::atomic<std::uint64_t> g_cameraDrawChanges{0};
std::atomic<std::uint64_t> g_cameraDrawStable{0};
std::atomic<std::uint64_t> g_cameraPresentSamples{0};
std::atomic<std::uint64_t> g_cameraPresentChanges{0};
std::atomic<std::uint64_t> g_cameraPresentRepeats{0};
std::atomic<std::uint64_t> g_cameraIdentityMismatches{0};
std::atomic<std::uint64_t> g_cameraStateMismatches{0};
std::atomic<std::uint64_t> g_cameraVariableIdentityMismatches{0};
std::atomic<std::uint64_t> g_cameraVariableStateMismatches{0};
std::atomic<std::uint64_t> g_cameraDrawIdentityMismatches{0};
std::atomic<std::uint64_t> g_cameraDrawStateMismatches{0};
std::atomic<std::uint64_t> g_cameraInvalidSamples{0};
std::atomic<std::uint64_t> g_listenerIdentityChanges{0};
std::atomic<std::uint64_t> g_cameraIdentityChanges{0};
std::atomic<std::uintptr_t> g_latestListenerPointer{0};
std::atomic<std::uintptr_t> g_latestCameraPointer{0};
camera_trace::Sample g_lastCompletedCamera{};
camera_trace::Sample g_lastVariableCamera{};
camera_trace::Sample g_lastDrawCamera{};
camera_trace::Sample g_lastPresentedCamera{};
bool g_hasLastCompletedCamera = false;
bool g_hasLastVariableCamera = false;
bool g_hasLastDrawCamera = false;
bool g_hasLastPresentedCamera = false;
camera::Runtime g_cameraRuntime;
std::atomic<std::uint64_t> g_cameraProbeStaged{0};
std::atomic<std::uint64_t> g_cameraProbeSkipped{0};
std::atomic<std::uint64_t> g_cameraProbeRestored{0};
std::atomic<std::uint64_t> g_cameraProbeIdentityChanges{0};
std::atomic<std::uint64_t> g_cameraProbeFailures{0};
clock::PresentationClock g_cameraClock;
double g_cameraClockStepSeconds = 0.0;
std::atomic<std::uint64_t> g_cameraInterpolationCaptures{0};
std::atomic<std::uint64_t> g_cameraInterpolationApplied{0};
std::atomic<std::uint64_t> g_cameraInterpolationRestored{0};
std::atomic<std::uint64_t> g_cameraInterpolationSkipped{0};
std::atomic<std::uint64_t> g_cameraInterpolationResets{0};
std::atomic<std::uint64_t> g_cameraInterpolationFailures{0};
// Render-side outcome of each camera transaction, indexed by
// camera::TransactionResult, so a skipped camera names its reason.
constexpr std::size_t kCameraStageResultCount = 18;
std::array<std::atomic<std::uint64_t>, kCameraStageResultCount>
    g_cameraStageResults{};

struct TracedEntityState {
    void* entity = nullptr;
    void* meshEntity = nullptr;
    hands_trace::Matrix latest{};
    hands_trace::Matrix previous{};
    hands_trace::Matrix rendered{};
    hands_trace::Matrix inverseRootTimesModel{};
    hands_trace::Matrix modelTimesInverseRoot{};
    float cameraDistance = 0.0F;
    float intervalInverseRootTimesModelMaxDelta = 0.0F;
    float intervalModelTimesInverseRootMaxDelta = 0.0F;
    std::uint64_t serial = 0;
    std::uint64_t endpointSerial = 0;
    std::uint64_t previousEndpointSerial = 0;
    std::uint64_t intervalWrites = 0;
    std::uint64_t intervalRenderMatches = 0;
    std::uint64_t intervalExactMatrixMatches = 0;
    std::uint64_t intervalDifferentMatrixMatches = 0;
    std::uint64_t intervalInverseRootTimesModelStable = 0;
    std::uint64_t intervalInverseRootTimesModelChanged = 0;
    std::uint64_t intervalModelTimesInverseRootStable = 0;
    std::uint64_t intervalModelTimesInverseRootChanged = 0;
    std::uint64_t intervalRelativeMatrixFailures = 0;
    bool hasLatest = false;
    bool hasPrevious = false;
    bool hasRendered = false;
    bool hasInverseRootTimesModel = false;
    bool hasModelTimesInverseRoot = false;
};

TracedEntityState g_handsRootState{};
constexpr std::size_t kAttachmentStateCapacity = 16;
constexpr std::size_t kSetMatrixMeshStateCapacity = 16;
std::array<TracedEntityState, kAttachmentStateCapacity> g_attachmentStates{};
std::array<TracedEntityState, kSetMatrixMeshStateCapacity>
    g_setMatrixMeshStates{};
std::uintptr_t g_handsRootMeshEntity = 0;

enum class RenderOwnerKind : std::uint8_t {
    setMatrix,
    attachment,
};

struct RenderOwnerEntry {
    std::uintptr_t owner = 0;
    std::uintptr_t entity = 0;
    std::uint32_t distanceMillis = 0;
    TracedEntityState* state = nullptr;
    RenderOwnerKind kind = RenderOwnerKind::setMatrix;
};

std::array<RenderOwnerEntry,
           kSetMatrixMeshStateCapacity + kAttachmentStateCapacity>
    g_renderOwners{};
std::size_t g_renderOwnerCount = 0;
std::atomic<std::uint64_t> g_entitySetMatrixCalls{0};
std::atomic<std::uint64_t> g_entitySetMatrixFinite{0};
std::atomic<std::uint64_t> g_entitySetMatrixCameraReady{0};
std::atomic<std::uint64_t> g_entitySetMatrixNearCamera{0};
std::atomic<std::uint64_t> g_entitySetMatrixWithinHalfMeter{0};
std::atomic<std::uint64_t> g_entitySetMatrixQuarterScale{0};
std::atomic<std::uint64_t> g_entitySetMatrixUnitScale{0};
std::atomic<std::uint64_t> g_entitySetMatrixOtherScale{0};
std::atomic<std::uint32_t> g_latestSetMatrixScaleXMillis{0};
std::atomic<std::uint32_t> g_latestSetMatrixScaleYMillis{0};
std::atomic<std::uint32_t> g_latestSetMatrixScaleZMillis{0};
std::atomic<std::uint32_t> g_latestSetMatrixDistanceMillis{0};
std::atomic<std::uintptr_t> g_latestSetMatrixEntity{0};
std::atomic<std::uint64_t> g_handsRootWrites{0};
std::atomic<std::uint64_t> g_handsRootChanges{0};
std::atomic<std::uint64_t> g_handsRootStable{0};
std::atomic<std::uint64_t> g_handsRootIdentityChanges{0};
std::atomic<std::uint64_t> g_handsRootRenderSamples{0};
std::atomic<std::uint64_t> g_handsRootRenderChanges{0};
std::atomic<std::uint64_t> g_handsRootRenderRepeats{0};
std::atomic<std::uint64_t> g_attachmentCalls{0};
std::atomic<std::uint64_t> g_attachmentNearCamera{0};
std::atomic<std::uint64_t> g_attachmentChanges{0};
std::atomic<std::uint64_t> g_attachmentStable{0};
std::atomic<std::uint64_t> g_attachmentIdentityChanges{0};
std::atomic<std::uint64_t> g_attachmentRenderSamples{0};
std::atomic<std::uint64_t> g_attachmentRenderChanges{0};
std::atomic<std::uint64_t> g_attachmentRenderRepeats{0};
std::atomic<std::uint64_t> g_attachmentTrackOverflows{0};
std::atomic<std::uintptr_t> g_latestAttachmentPointer{0};
std::atomic<std::uint64_t> g_handsTraceInvalid{0};
std::atomic<std::uint64_t> g_setMatrixMeshWrites{0};
std::atomic<std::uint64_t> g_setMatrixMeshChanges{0};
std::atomic<std::uint64_t> g_setMatrixMeshStable{0};
std::atomic<std::uint64_t> g_setMatrixMeshIdentityChanges{0};
std::atomic<std::uint64_t> g_setMatrixMeshTrackOverflows{0};
std::atomic<std::uint64_t> g_subMeshModelSetMatrixMatches{0};
std::atomic<std::uint64_t> g_subMeshModelAttachmentMatches{0};
std::atomic<std::uint64_t> g_handsRenderCopyReturns{0};
std::atomic<std::uint64_t> g_handsInterpolationApplied{0};
std::atomic<std::uint64_t> g_handsInterpolationSkipped{0};
std::atomic<std::uint64_t> g_handsInterpolationFailures{0};
std::atomic<std::uint64_t> g_handsPoseSkinCalls{0};
std::atomic<std::uint64_t> g_handsPoseOwnerMatches{0};
std::atomic<std::uint64_t> g_handsPosePaletteValid{0};
std::atomic<std::uint64_t> g_handsPosePaletteEmpty{0};
std::atomic<std::uint64_t> g_handsPosePaletteInvalid{0};
std::atomic<std::uint64_t> g_handsPoseNearSamples{0};
std::atomic<std::uint64_t> g_handsPoseTrackOverflows{0};
std::atomic<std::uint64_t> g_handsPoseRendererCalls{0};
std::atomic<std::uint64_t> g_handsPoseRendererOwnerMatches{0};
std::atomic<std::uint64_t> g_handsPoseRendererPaletteValid{0};
std::atomic<std::uint64_t> g_handsPoseRendererPaletteInvalid{0};
std::atomic<std::uint64_t> g_handsPoseRendererPointerMismatches{0};
std::atomic<std::uint64_t> g_handsPoseRendererCopyReturns{0};
std::atomic<std::uint64_t> g_handsPoseRendererCopyFailures{0};
std::atomic<std::uint64_t> g_handsPoseProbeArmed{0};
std::atomic<std::uint64_t> g_handsPoseProbeUpdaterMatches{0};
std::atomic<std::uint64_t> g_handsPoseProbeStaged{0};
std::atomic<std::uint64_t> g_handsPoseProbeRestored{0};
std::atomic<std::uint64_t> g_handsPoseProbeFailures{0};
std::atomic<std::uint64_t> g_handsPoseProbeVerifyMismatches{0};
std::atomic<std::uint64_t> g_handsPoseInterpolationApplied{0};
std::atomic<std::uint64_t> g_handsPoseInterpolationReturns{0};
std::atomic<std::uint64_t> g_handsPoseInterpolationSkipped{0};
std::atomic<std::uint64_t> g_handsPoseInterpolationFailures{0};
std::atomic<std::uintptr_t> g_latestSetMatrixRenderOwner{0};
std::atomic<std::uintptr_t> g_latestSetMatrixRenderEntity{0};
std::atomic<std::uint32_t> g_latestSetMatrixRenderDistanceMillis{0};
alignas(16) thread_local hands_trace::Matrix g_handsRenderMatrixCopy{};
clock::Sample g_activeHandsClockSample{};
bool g_hasActiveHandsClockSample = false;
std::atomic<std::uint64_t> g_handsProbeStagedFrames{0};
std::atomic<std::uint64_t> g_handsProbeStagedEntities{0};
std::atomic<std::uint64_t> g_handsProbeSkippedFrames{0};
std::atomic<std::uint64_t> g_handsProbeRestoredFrames{0};
std::atomic<std::uint64_t> g_handsProbeRestoredEntities{0};
std::atomic<std::uint64_t> g_handsProbeFailures{0};
std::atomic<std::uint64_t> g_handsProbeInitialReadFailures{0};
std::atomic<std::uint64_t> g_handsProbeWriteFailures{0};
std::atomic<std::uint64_t> g_handsProbeVerifyReadFailures{0};
std::atomic<std::uint64_t> g_handsProbeVerifyMismatches{0};
std::atomic<std::uintptr_t> g_handsProbeLatestStagedEntity{0};
std::atomic<std::uint32_t> g_handsProbeLatestStagedDistanceMillis{0};
std::atomic<std::uintptr_t> g_handsProbeLatestFailureEntity{0};
std::atomic<std::uint32_t> g_handsProbeLatestFailureDistanceMillis{0};
std::atomic<std::uint64_t> g_handsProbeExcludedEntities{0};
std::array<void*, kAttachmentStateCapacity + 1> g_handsProbeRejectedEntities{};
std::size_t g_handsProbeRejectedCount = 0;

struct EntityProbeEntry {
    void* entity = nullptr;
    hands_trace::Matrix original{};
};

struct EntityProbeTransaction {
    std::array<EntityProbeEntry, kAttachmentStateCapacity + 1> entries{};
    std::size_t count = 0;
    bool active = false;
};

EntityProbeTransaction g_handsProbeTransaction{};

struct HandsPoseTraceState {
    void* owner = nullptr;
    std::uint64_t lastHash = 0;
    std::uint64_t lastRenderSerial = 0;
    std::uint64_t lastRestoreSerial = 0;
    std::uint64_t lastRendererHash = 0;
    std::uint64_t lastRendererRenderSerial = 0;
    std::uint64_t lastInterpolationRenderSerial = 0;
    std::uint64_t intervalSamples = 0;
    std::uint64_t intervalChanges = 0;
    std::uint64_t intervalRepeats = 0;
    std::uint64_t intervalBoneCountChanges = 0;
    std::uint64_t intervalRenderOwnerMatches = 0;
    std::uint64_t intervalRendererSamples = 0;
    std::uint64_t intervalRendererChanges = 0;
    std::uint64_t intervalRendererRepeats = 0;
    std::size_t boneCount = 0;
    std::size_t maximumBoneCount = 0;
    std::size_t rendererBoneCount = 0;
    std::size_t maximumRendererBoneCount = 0;
    float modelDistance = 0.0F;
    std::uintptr_t renderEntity = 0;
    RenderOwnerKind renderKind = RenderOwnerKind::setMatrix;
    const void* rendererPaletteBuffer = nullptr;
    hands_pose::History rendererPoseHistory{};
    // Returned to the renderer for the rest of the frame, so it is owned by
    // the slot rather than shared between owners or threads.
    alignas(16) std::array<hands_pose::DualQuaternion,
                           hands_pose::kMaximumBones>
        presentedRendererPalette{};
    std::size_t presentedBoneCount = 0;
    std::uint64_t lastUsedRenderSerial = 0;
    bool hasHash = false;
    bool hasRendererHash = false;
    bool rendererInterpolationReady = false;
};

// First-person owners are the mesh-bearing entities the game repositions
// with iLuxEntity::SetMatrix close to the camera: the held-item rig and the
// full player body whose arms are the visible hands. The body root sits at
// the feet, so the bound has to cover a standing camera height.
constexpr std::uint32_t kFirstPersonOwnerMaxDistanceMillis = 2000U;

constexpr std::size_t kHandsPoseTraceCapacity = 32;
constexpr std::size_t kMaximumTracedBones = 512;
std::array<HandsPoseTraceState, kHandsPoseTraceCapacity> g_handsPoseStates{};
std::uint64_t g_handsPoseRenderSerial = 0;
std::atomic<bool> g_handsPoseRenderActive{false};

// The renderer calls the model-matrix and palette getters from several engine
// threads while RenderAll is running. Owner tables and the clock sample are
// frozen before RenderAll starts; everything those callbacks mutate (trace
// slots, pose history, interval counters) is serialized by this lock, which
// is taken only after the cheap owner match.
std::atomic_flag g_renderStateBusy;

class RenderStateLock final {
public:
    RenderStateLock() noexcept {
        while (g_renderStateBusy.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
    }
    ~RenderStateLock() { g_renderStateBusy.clear(std::memory_order_release); }
    RenderStateLock(const RenderStateLock&) = delete;
    RenderStateLock& operator=(const RenderStateLock&) = delete;
};

struct HandsPoseProbeTransaction {
    void* owner = nullptr;
    hands_trace::Matrix* palette = nullptr;
    std::size_t count = 0;
    std::array<hands_trace::Matrix, kMaximumTracedBones> original{};
    std::array<hands_trace::Matrix, kMaximumTracedBones> presented{};
    bool armed = false;
    bool staged = false;
};

thread_local HandsPoseProbeTransaction g_handsPoseProbeTransaction{};

constexpr DWORD kCreateWaitableTimerHighResolution = 0x00000002;

std::uint64_t g_lastPhaseSerial = 0;
double g_lastPhaseAlpha = 0.0;
bool g_hasLastPhase = false;

void SampleCompletedCamera() {
    if (!g_motionTraceEnabled) {
        return;
    }
    camera_trace::Sample sample{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, sample) !=
        camera_trace::SampleStatus::ok) {
        g_cameraInvalidSamples.fetch_add(1, std::memory_order_relaxed);
        g_hasLastCompletedCamera = false;
        return;
    }
    g_cameraCompletedSamples.fetch_add(1, std::memory_order_relaxed);
    g_latestListenerPointer.store(sample.listener, std::memory_order_relaxed);
    g_latestCameraPointer.store(sample.camera, std::memory_order_relaxed);
    if (g_hasLastCompletedCamera) {
        if (sample.listener != g_lastCompletedCamera.listener) {
            g_listenerIdentityChanges.fetch_add(1, std::memory_order_relaxed);
        }
        if (sample.camera != g_lastCompletedCamera.camera) {
            g_cameraIdentityChanges.fetch_add(1, std::memory_order_relaxed);
        } else if (camera_trace::SameTransform(sample, g_lastCompletedCamera)) {
            g_cameraCompletedStable.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cameraCompletedChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_lastCompletedCamera = sample;
    g_hasLastCompletedCamera = true;
}

void SampleVariableCamera() {
    if (!g_motionTraceEnabled) {
        return;
    }
    camera_trace::Sample sample{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, sample) !=
        camera_trace::SampleStatus::ok) {
        g_cameraInvalidSamples.fetch_add(1, std::memory_order_relaxed);
        g_hasLastVariableCamera = false;
        if (g_mouseTraceEnabled) {
            mouse::OnCameraTick(g_fixedSerial.load(std::memory_order_relaxed),
                                0.0F, 0.0F, nullptr, false);
        }
        return;
    }
    g_cameraVariableSamples.fetch_add(1, std::memory_order_relaxed);
    if (g_hasLastVariableCamera &&
        camera_trace::SameIdentity(sample, g_lastVariableCamera)) {
        if (camera_trace::SameTransform(sample, g_lastVariableCamera)) {
            g_cameraVariableStable.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cameraVariableChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_lastVariableCamera = sample;
    g_hasLastVariableCamera = true;
    if (g_mouseTraceEnabled) {
        // The yaw and pitch fields only describe Euler-mode cameras.
        std::uint32_t rotateMode = 1;
        std::memcpy(&rotateMode,
                    reinterpret_cast<const std::uint8_t*>(sample.camera) + 0x70,
                    sizeof(rotateMode));
        const float* view = nullptr;
        if (rotateMode == 0 && g_mousePreview &&
            g_cameraGetViewMatrix != nullptr) {
            // The engine's own getter: it refreshes the cached view for the
            // authoritative pose, exactly as the next reader would have.
            view = g_cameraGetViewMatrix(
                reinterpret_cast<void*>(sample.camera));
        }
        mouse::OnCameraTick(g_fixedSerial.load(std::memory_order_relaxed),
                            sample.yaw, sample.pitch, view, rotateMode == 0);
    }
}

void SampleDrawCamera() {
    if (!g_motionTraceEnabled) {
        return;
    }
    camera_trace::Sample sample{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, sample) !=
        camera_trace::SampleStatus::ok) {
        g_cameraInvalidSamples.fetch_add(1, std::memory_order_relaxed);
        g_hasLastDrawCamera = false;
        return;
    }
    g_cameraDrawSamples.fetch_add(1, std::memory_order_relaxed);
    // A listener without a world is a menu or a loading screen.
    std::uintptr_t world = 0;
    std::memcpy(&world,
                reinterpret_cast<const std::uint8_t*>(sample.listener) + 0x20,
                sizeof(world));
    if (world != 0) {
        frame_trace::MarkGameplay();
    }
    if (g_hasLastDrawCamera && camera_trace::SameIdentity(sample, g_lastDrawCamera)) {
        if (camera_trace::SameTransform(sample, g_lastDrawCamera)) {
            g_cameraDrawStable.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cameraDrawChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_lastDrawCamera = sample;
    g_hasLastDrawCamera = true;
}

void SamplePresentedCamera() {
    if (!g_motionTraceEnabled) {
        return;
    }
    camera_trace::Sample sample{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, sample) !=
        camera_trace::SampleStatus::ok) {
        g_cameraInvalidSamples.fetch_add(1, std::memory_order_relaxed);
        g_hasLastPresentedCamera = false;
        return;
    }
    g_cameraPresentSamples.fetch_add(1, std::memory_order_relaxed);
    g_latestListenerPointer.store(sample.listener, std::memory_order_relaxed);
    g_latestCameraPointer.store(sample.camera, std::memory_order_relaxed);
    if (g_hasLastPresentedCamera &&
        camera_trace::SameIdentity(sample, g_lastPresentedCamera)) {
        if (camera_trace::SameTransform(sample, g_lastPresentedCamera)) {
            g_cameraPresentRepeats.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cameraPresentChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_hasLastCompletedCamera) {
        if (!camera_trace::SameIdentity(sample, g_lastCompletedCamera)) {
            g_cameraIdentityMismatches.fetch_add(1, std::memory_order_relaxed);
        } else if (!camera_trace::SameTransform(sample, g_lastCompletedCamera)) {
            g_cameraStateMismatches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_hasLastVariableCamera) {
        if (!camera_trace::SameIdentity(sample, g_lastVariableCamera)) {
            g_cameraVariableIdentityMismatches.fetch_add(
                1, std::memory_order_relaxed);
        } else if (!camera_trace::SameTransform(sample, g_lastVariableCamera)) {
            g_cameraVariableStateMismatches.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    if (g_hasLastDrawCamera) {
        if (!camera_trace::SameIdentity(sample, g_lastDrawCamera)) {
            g_cameraDrawIdentityMismatches.fetch_add(
                1, std::memory_order_relaxed);
        } else if (!camera_trace::SameTransform(sample, g_lastDrawCamera)) {
            g_cameraDrawStateMismatches.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    g_lastPresentedCamera = sample;
    g_hasLastPresentedCamera = true;
}

std::wstring MinHookError(const wchar_t* operation, const MH_STATUS status) {
    const char* text = MH_StatusToString(status);
    std::wstring converted;
    if (text != nullptr) {
        while (*text != '\0') {
            converted.push_back(static_cast<unsigned char>(*text));
            ++text;
        }
    }
    return std::wstring(operation) + L" failed (" + std::to_wstring(status) +
           L", " + converted + L")";
}

struct HandsPaletteView {
    const hands_trace::Matrix* begin = nullptr;
    std::size_t count = 0;
};

enum class HandsPaletteStatus : std::uint8_t {
    empty,
    valid,
    invalid,
};

HandsPaletteStatus ReadHandsPalette(void* const owner,
                                    HandsPaletteView& view) noexcept {
    view = {};
    constexpr std::size_t beginOffset = 0x300;
    constexpr std::size_t endOffset = 0x308;
    constexpr std::size_t capacityOffset = 0x310;
    const hands_trace::Matrix* begin = nullptr;
    const hands_trace::Matrix* end = nullptr;
    const hands_trace::Matrix* capacity = nullptr;
    const auto* const base = static_cast<const std::uint8_t*>(owner);
    if (owner == nullptr || !memory::Read(base + beginOffset, begin) ||
        !memory::Read(base + endOffset, end) ||
        !memory::Read(base + capacityOffset, capacity)) {
        return HandsPaletteStatus::invalid;
    }
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    const auto capacityAddress = reinterpret_cast<std::uintptr_t>(capacity);
    if (begin == nullptr && end == nullptr && capacity == nullptr) {
        return HandsPaletteStatus::empty;
    }
    if (begin == nullptr || endAddress < beginAddress ||
        capacityAddress < endAddress) {
        return HandsPaletteStatus::invalid;
    }
    const std::uintptr_t bytes = endAddress - beginAddress;
    const std::uintptr_t capacityBytes = capacityAddress - beginAddress;
    if (bytes % sizeof(hands_trace::Matrix) != 0 ||
        capacityBytes % sizeof(hands_trace::Matrix) != 0) {
        return HandsPaletteStatus::invalid;
    }
    const std::size_t count =
        static_cast<std::size_t>(bytes / sizeof(hands_trace::Matrix));
    if (count == 0) {
        return HandsPaletteStatus::empty;
    }
    if (count > kMaximumTracedBones) {
        return HandsPaletteStatus::invalid;
    }
    bool finite = true;
    if (!memory::Guarded([&]() noexcept {
            for (std::size_t index = 0; index < count && finite; ++index) {
                finite = hands_trace::IsFiniteAffine(begin[index]);
            }
        }) ||
        !finite) {
        return HandsPaletteStatus::invalid;
    }
    view = {begin, count};
    return HandsPaletteStatus::valid;
}

std::uint64_t HashHandsPalette(const HandsPaletteView& view) noexcept {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(view.begin);
    const std::size_t byteCount = view.count * sizeof(hands_trace::Matrix);
    std::uint64_t hash = offsetBasis;
    for (std::size_t index = 0; index < byteCount; ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

constexpr std::size_t kFirstPersonHandsBoneCount = 23;

using SkinningDualQuaternion = hands_pose::DualQuaternion;

// Used only by the retired-on-success HandsPoseRenderCopyProbe.
alignas(16) thread_local std::array<SkinningDualQuaternion,
                                    kFirstPersonHandsBoneCount>
    g_handsPoseRendererCopy{};

struct RendererPaletteView {
    const SkinningDualQuaternion* begin = nullptr;
    std::size_t count = 0;
};

HandsPaletteStatus ReadRendererPalette(void* const owner,
                                       RendererPaletteView& view) noexcept {
    view = {};
    constexpr std::size_t beginOffset = 0x318;
    constexpr std::size_t endOffset = 0x320;
    constexpr std::size_t capacityOffset = 0x328;
    const SkinningDualQuaternion* begin = nullptr;
    const SkinningDualQuaternion* end = nullptr;
    const SkinningDualQuaternion* capacity = nullptr;
    const auto* const base = static_cast<const std::uint8_t*>(owner);
    if (owner == nullptr || !memory::Read(base + beginOffset, begin) ||
        !memory::Read(base + endOffset, end) ||
        !memory::Read(base + capacityOffset, capacity)) {
        return HandsPaletteStatus::invalid;
    }
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    const auto capacityAddress = reinterpret_cast<std::uintptr_t>(capacity);
    if (begin == nullptr && end == nullptr && capacity == nullptr) {
        return HandsPaletteStatus::empty;
    }
    if (begin == nullptr || endAddress < beginAddress ||
        capacityAddress < endAddress) {
        return HandsPaletteStatus::invalid;
    }
    const std::uintptr_t bytes = endAddress - beginAddress;
    const std::uintptr_t capacityBytes = capacityAddress - beginAddress;
    if (bytes % sizeof(SkinningDualQuaternion) != 0 ||
        capacityBytes % sizeof(SkinningDualQuaternion) != 0) {
        return HandsPaletteStatus::invalid;
    }
    const std::size_t count = static_cast<std::size_t>(
        bytes / sizeof(SkinningDualQuaternion));
    if (count == 0) {
        return HandsPaletteStatus::empty;
    }
    if (count > kMaximumTracedBones) {
        return HandsPaletteStatus::invalid;
    }
    bool finite = true;
    if (!memory::Guarded([&]() noexcept {
            for (std::size_t index = 0; index < count && finite; ++index) {
                for (const float component : begin[index].values) {
                    finite = finite && std::isfinite(component);
                }
            }
        }) ||
        !finite) {
        return HandsPaletteStatus::invalid;
    }
    view = {begin, count};
    return HandsPaletteStatus::valid;
}

std::uint64_t HashRendererPalette(const RendererPaletteView& view) noexcept {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(view.begin);
    const std::size_t byteCount =
        view.count * sizeof(SkinningDualQuaternion);
    std::uint64_t hash = offsetBasis;
    for (std::size_t index = 0; index < byteCount; ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

// Mesh owners come and go as items are drawn, used, and put away. A slot
// whose owner has not been rendered for this many scene renders is reused,
// otherwise the table fills and later owners silently lose interpolation.
constexpr std::uint64_t kHandsPoseStaleRenders = 600;

HandsPoseTraceState* FindOrCreateHandsPoseState(void* const owner) noexcept {
    HandsPoseTraceState* freeSlot = nullptr;
    HandsPoseTraceState* stalest = nullptr;
    for (auto& state : g_handsPoseStates) {
        if (state.owner == owner) {
            state.lastUsedRenderSerial = g_handsPoseRenderSerial;
            return &state;
        }
        if (state.owner == nullptr) {
            if (freeSlot == nullptr) {
                freeSlot = &state;
            }
        } else if (g_handsPoseRenderSerial - state.lastUsedRenderSerial >
                       kHandsPoseStaleRenders &&
                   (stalest == nullptr ||
                    state.lastUsedRenderSerial <
                        stalest->lastUsedRenderSerial)) {
            stalest = &state;
        }
    }
    if (freeSlot == nullptr && stalest != nullptr) {
        *stalest = HandsPoseTraceState{};
        freeSlot = stalest;
    }
    if (freeSlot != nullptr) {
        freeSlot->owner = owner;
        freeSlot->lastUsedRenderSerial = g_handsPoseRenderSerial;
    }
    return freeSlot;
}

const RenderOwnerEntry* FindKnownRenderOwner(
    const std::uintptr_t owner) noexcept {
    for (std::size_t index = 0; index < g_renderOwnerCount; ++index) {
        if (g_renderOwners[index].owner == owner) {
            return &g_renderOwners[index];
        }
    }
    return nullptr;
}

void BuildInterpolatedRendererPose(
    HandsPoseTraceState& state,
    const RendererPaletteView& palette) noexcept {
    state.lastInterpolationRenderSerial = g_handsPoseRenderSerial;
    state.rendererInterpolationReady = false;
    if (!g_hasActiveHandsClockSample ||
        g_activeHandsClockSample.kind != clock::SampleKind::interpolated ||
        palette.count > hands_pose::kMaximumBones) {
        g_handsPoseInterpolationSkipped.fetch_add(1,
                                                  std::memory_order_relaxed);
        return;
    }

    hands_pose::Pose authoritative{};
    authoritative.boneCount = palette.count;
    std::memcpy(authoritative.bones.data(), palette.begin,
                palette.count * sizeof(SkinningDualQuaternion));
    const std::uint64_t serial =
        g_fixedSerial.load(std::memory_order_relaxed);
    const hands_pose::PublishKind published =
        state.rendererPoseHistory.Publish(
            serial, g_activeHandsClockSample.generation, authoritative);
    if (published == hands_pose::PublishKind::invalid) {
        g_handsPoseInterpolationFailures.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    const hands_pose::SampleResult sampled =
        state.rendererPoseHistory.Evaluate(g_activeHandsClockSample);
    if (sampled.kind != hands_pose::SampleKind::applied ||
        sampled.pose.boneCount != palette.count) {
        if (sampled.kind == hands_pose::SampleKind::invalid) {
            g_handsPoseInterpolationFailures.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            g_handsPoseInterpolationSkipped.fetch_add(
                1, std::memory_order_relaxed);
        }
        return;
    }

    std::memcpy(state.presentedRendererPalette.data(),
                sampled.pose.bones.data(),
                palette.count * sizeof(SkinningDualQuaternion));
    state.presentedBoneCount = palette.count;
    state.rendererInterpolationReady = true;
    g_handsPoseInterpolationApplied.fetch_add(1,
                                              std::memory_order_relaxed);
}

void RecordRendererPalette(void* const subMesh,
                           const void* const returnedPalette) noexcept {
    if (!g_handsPoseRenderActive || subMesh == nullptr ||
        returnedPalette == nullptr) {
        return;
    }
    constexpr std::size_t ownerOffset = 0x1B0;
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) + ownerOffset,
                sizeof(owner));
    const RenderOwnerEntry* const renderOwner = FindKnownRenderOwner(
        reinterpret_cast<std::uintptr_t>(owner));
    if (owner == nullptr || renderOwner == nullptr) {
        return;
    }
    g_handsPoseRendererOwnerMatches.fetch_add(1,
                                              std::memory_order_relaxed);
    HandsPoseTraceState* const state = FindOrCreateHandsPoseState(owner);
    if (state == nullptr) {
        g_handsPoseTrackOverflows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (state->lastRendererRenderSerial == g_handsPoseRenderSerial) {
        return;
    }
    state->lastRendererRenderSerial = g_handsPoseRenderSerial;
    state->modelDistance =
        static_cast<float>(renderOwner->distanceMillis) / 1000.0F;
    state->renderEntity = renderOwner->entity;
    state->renderKind = renderOwner->kind;

    RendererPaletteView palette{};
    if (ReadRendererPalette(owner, palette) != HandsPaletteStatus::valid) {
        g_handsPoseRendererPaletteInvalid.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    g_handsPoseRendererPaletteValid.fetch_add(1,
                                              std::memory_order_relaxed);
    if (returnedPalette != palette.begin) {
        g_handsPoseRendererPointerMismatches.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    if (state->rendererPaletteBuffer != palette.begin) {
        state->rendererPaletteBuffer = palette.begin;
        state->hasRendererHash = false;
        state->rendererPoseHistory.Reset();
        state->rendererInterpolationReady = false;
    }
    const std::uint64_t hash = HashRendererPalette(palette);
    ++state->intervalRendererSamples;
    if (state->hasRendererHash) {
        if (state->lastRendererHash == hash) {
            ++state->intervalRendererRepeats;
        } else {
            ++state->intervalRendererChanges;
        }
    }
    state->lastRendererHash = hash;
    state->hasRendererHash = true;
    state->rendererBoneCount = palette.count;
    state->maximumRendererBoneCount =
        std::max(state->maximumRendererBoneCount, palette.count);
    if (g_handsPoseInterpolation &&
        renderOwner->kind == RenderOwnerKind::setMatrix &&
        renderOwner->distanceMillis <= kFirstPersonOwnerMaxDistanceMillis) {
        BuildInterpolatedRendererPose(*state, palette);
    }
}

const void* CopyFirstPersonRendererPalette(
    void* const subMesh,
    const void* const returnedPalette) noexcept {
    if (!g_handsPoseRenderCopyProbe || !g_handsPoseRenderActive ||
        subMesh == nullptr || returnedPalette == nullptr) {
        return returnedPalette;
    }
    constexpr std::size_t ownerOffset = 0x1B0;
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) + ownerOffset,
                sizeof(owner));
    const RenderOwnerEntry* const renderOwner = FindKnownRenderOwner(
        reinterpret_cast<std::uintptr_t>(owner));
    if (owner == nullptr || renderOwner == nullptr ||
        renderOwner->distanceMillis > 1000U) {
        return returnedPalette;
    }
    constexpr std::size_t beginOffset = 0x318;
    constexpr std::size_t endOffset = 0x320;
    const SkinningDualQuaternion* begin = nullptr;
    const SkinningDualQuaternion* end = nullptr;
    std::memcpy(&begin,
                static_cast<const std::uint8_t*>(owner) + beginOffset,
                sizeof(begin));
    std::memcpy(&end,
                static_cast<const std::uint8_t*>(owner) + endOffset,
                sizeof(end));
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (begin == nullptr || endAddress < beginAddress ||
        endAddress - beginAddress != sizeof(g_handsPoseRendererCopy)) {
        return returnedPalette;
    }
    if (begin != returnedPalette) {
        g_handsPoseRendererCopyFailures.fetch_add(
            1, std::memory_order_relaxed);
        return returnedPalette;
    }
    std::memcpy(g_handsPoseRendererCopy.data(), returnedPalette,
                sizeof(g_handsPoseRendererCopy));
    g_handsPoseRendererCopyReturns.fetch_add(1,
                                             std::memory_order_relaxed);
    return g_handsPoseRendererCopy.data();
}

const void* InterpolateFirstPersonRendererPalette(
    void* const subMesh,
    const void* const returnedPalette) noexcept {
    if (!g_handsPoseInterpolation || !g_handsPoseRenderActive ||
        subMesh == nullptr || returnedPalette == nullptr) {
        return returnedPalette;
    }
    constexpr std::size_t ownerOffset = 0x1B0;
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) + ownerOffset,
                sizeof(owner));
    const RenderOwnerEntry* const renderOwner = FindKnownRenderOwner(
        reinterpret_cast<std::uintptr_t>(owner));
    if (owner == nullptr || renderOwner == nullptr ||
        renderOwner->kind != RenderOwnerKind::setMatrix ||
        renderOwner->distanceMillis > kFirstPersonOwnerMaxDistanceMillis) {
        return returnedPalette;
    }

    HandsPoseTraceState* const state = FindOrCreateHandsPoseState(owner);
    if (state == nullptr ||
        state->lastInterpolationRenderSerial != g_handsPoseRenderSerial ||
        !state->rendererInterpolationReady ||
        state->rendererPaletteBuffer != returnedPalette) {
        return returnedPalette;
    }

    // The live vector must still be exactly the palette the pose was built
    // from this frame; anything else keeps the engine's own pointer.
    constexpr std::size_t beginOffset = 0x318;
    constexpr std::size_t endOffset = 0x320;
    std::uintptr_t beginAddress = 0;
    std::uintptr_t endAddress = 0;
    std::memcpy(&beginAddress,
                static_cast<const std::uint8_t*>(owner) + beginOffset,
                sizeof(beginAddress));
    std::memcpy(&endAddress,
                static_cast<const std::uint8_t*>(owner) + endOffset,
                sizeof(endAddress));
    if (beginAddress != reinterpret_cast<std::uintptr_t>(returnedPalette) ||
        endAddress < beginAddress ||
        endAddress - beginAddress !=
            state->presentedBoneCount * sizeof(SkinningDualQuaternion)) {
        g_handsPoseInterpolationFailures.fetch_add(
            1, std::memory_order_relaxed);
        return returnedPalette;
    }

    g_handsPoseInterpolationReturns.fetch_add(1,
                                              std::memory_order_relaxed);
    return state->presentedRendererPalette.data();
}

void ArmHandsPoseProbe(void* const subMesh) noexcept {
    if (!g_handsPoseRestoreProbe || !g_handsPoseRenderActive ||
        subMesh == nullptr || g_handsPoseProbeTransaction.armed) {
        return;
    }
    constexpr std::size_t ownerOffset = 0x1B0;
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) + ownerOffset,
                sizeof(owner));
    const RenderOwnerEntry* const renderOwner = FindKnownRenderOwner(
        reinterpret_cast<std::uintptr_t>(owner));
    if (owner == nullptr || renderOwner == nullptr ||
        renderOwner->distanceMillis > 1000) {
        return;
    }
    HandsPaletteView palette{};
    if (ReadHandsPalette(owner, palette) != HandsPaletteStatus::valid ||
        palette.count != kFirstPersonHandsBoneCount) {
        return;
    }
    {
        const RenderStateLock lock;
        HandsPoseTraceState* const state = FindOrCreateHandsPoseState(owner);
        if (state == nullptr ||
            state->lastRestoreSerial == g_handsPoseRenderSerial) {
            return;
        }
    }
    auto& transaction = g_handsPoseProbeTransaction;
    transaction.owner = owner;
    transaction.palette = nullptr;
    transaction.count = 0;
    transaction.armed = true;
    transaction.staged = false;
    g_handsPoseProbeArmed.fetch_add(1, std::memory_order_relaxed);
}

void StageHandsPoseProbe(void* const owner) noexcept {
    auto& transaction = g_handsPoseProbeTransaction;
    if (!g_handsPoseRestoreProbe || !transaction.armed ||
        transaction.staged || owner != transaction.owner) {
        return;
    }
    g_handsPoseProbeUpdaterMatches.fetch_add(1, std::memory_order_relaxed);
    HandsPaletteView palette{};
    if (ReadHandsPalette(owner, palette) != HandsPaletteStatus::valid ||
        palette.count != kFirstPersonHandsBoneCount) {
        g_handsPoseProbeFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t bytes = palette.count * sizeof(hands_trace::Matrix);
    auto* const writable = const_cast<hands_trace::Matrix*>(palette.begin);
    if (!memory::Copy(transaction.original.data(), palette.begin, bytes)) {
        g_handsPoseProbeFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(transaction.presented.data(), transaction.original.data(),
                bytes);
    transaction.palette = writable;
    transaction.count = palette.count;
    if (!memory::Copy(writable, transaction.presented.data(), bytes) ||
        !memory::Equal(writable, transaction.presented.data(), bytes)) {
        g_handsPoseProbeVerifyMismatches.fetch_add(
            1, std::memory_order_relaxed);
        g_handsPoseProbeFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    transaction.staged = true;
    g_handsPoseProbeStaged.fetch_add(1, std::memory_order_relaxed);
}

void FinishHandsPoseProbe() noexcept {
    auto& transaction = g_handsPoseProbeTransaction;
    if (!transaction.armed) {
        return;
    }
    bool restored = false;
    if (!transaction.staged || transaction.palette == nullptr ||
        transaction.count != kFirstPersonHandsBoneCount) {
        g_handsPoseProbeFailures.fetch_add(1, std::memory_order_relaxed);
    } else {
        const std::size_t bytes =
            transaction.count * sizeof(hands_trace::Matrix);
        if (!memory::Equal(transaction.palette,
                           transaction.presented.data(), bytes)) {
            g_handsPoseProbeVerifyMismatches.fetch_add(
                1, std::memory_order_relaxed);
            g_handsPoseProbeFailures.fetch_add(1,
                                               std::memory_order_relaxed);
        } else {
            if (memory::Copy(transaction.palette,
                             transaction.original.data(), bytes) &&
                memory::Equal(transaction.palette,
                              transaction.original.data(), bytes)) {
                restored = true;
                g_handsPoseProbeRestored.fetch_add(1,
                                                   std::memory_order_relaxed);
            } else {
                g_handsPoseProbeVerifyMismatches.fetch_add(
                    1, std::memory_order_relaxed);
                g_handsPoseProbeFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }
    if (restored) {
        const RenderStateLock lock;
        HandsPoseTraceState* const state =
            FindOrCreateHandsPoseState(transaction.owner);
        if (state != nullptr) {
            state->lastRestoreSerial = g_handsPoseRenderSerial;
        }
    }
    transaction.owner = nullptr;
    transaction.palette = nullptr;
    transaction.count = 0;
    transaction.armed = false;
    transaction.staged = false;
}

void RecordHandsPosePalette(void* const subMesh) noexcept {
    if (!g_handsPoseRenderActive || subMesh == nullptr) {
        return;
    }
    g_handsPoseSkinCalls.fetch_add(1, std::memory_order_relaxed);
    constexpr std::size_t ownerOffset = 0x1B0;
    void* owner = nullptr;
    // The original callback has just dereferenced this live submesh and its
    // +0x1B0 owner, so the same proven field is read directly in this
    // exceptionally hot renderer callback.
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) + ownerOffset,
                sizeof(owner));
    if (owner == nullptr) {
        return;
    }
    const RenderOwnerEntry* const renderOwner =
        FindKnownRenderOwner(reinterpret_cast<std::uintptr_t>(owner));
    if (renderOwner == nullptr || renderOwner->distanceMillis > 2000) {
        return;
    }
    const RenderStateLock lock;
    g_handsPoseOwnerMatches.fetch_add(1, std::memory_order_relaxed);
    HandsPoseTraceState* const state = FindOrCreateHandsPoseState(owner);
    if (state == nullptr) {
        g_handsPoseTrackOverflows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (state->lastRenderSerial == g_handsPoseRenderSerial) {
        return;
    }
    state->lastRenderSerial = g_handsPoseRenderSerial;
    state->modelDistance =
        static_cast<float>(renderOwner->distanceMillis) / 1000.0F;
    state->renderEntity = renderOwner->entity;
    state->renderKind = renderOwner->kind;

    HandsPaletteView palette{};
    const HandsPaletteStatus paletteStatus = ReadHandsPalette(owner, palette);
    if (paletteStatus == HandsPaletteStatus::empty) {
        g_handsPosePaletteEmpty.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (paletteStatus != HandsPaletteStatus::valid) {
        g_handsPosePaletteInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_handsPosePaletteValid.fetch_add(1, std::memory_order_relaxed);
    ++state->intervalSamples;
    state->maximumBoneCount = std::max(state->maximumBoneCount, palette.count);
    if (state->boneCount != 0 && state->boneCount != palette.count) {
        ++state->intervalBoneCountChanges;
        state->hasHash = false;
    }
    state->boneCount = palette.count;
    ++state->intervalRenderOwnerMatches;
    const std::uint64_t hash = HashHandsPalette(palette);
    if (state->hasHash) {
        if (state->lastHash == hash) {
            ++state->intervalRepeats;
        } else {
            ++state->intervalChanges;
        }
    }
    state->lastHash = hash;
    state->hasHash = true;
    g_handsPoseNearSamples.fetch_add(1, std::memory_order_relaxed);
}

void RecordTrackedEntity(TracedEntityState& state,
                         void* const entity,
                         const hands_trace::Matrix& matrix,
                         std::atomic<std::uint64_t>& changes,
                         std::atomic<std::uint64_t>& stable,
                         std::atomic<std::uint64_t>& identityChanges) {
    if (state.hasLatest && state.entity != entity) {
        identityChanges.fetch_add(1, std::memory_order_relaxed);
        state.hasRendered = false;
    }
    if (state.hasLatest && state.entity == entity) {
        (hands_trace::SameMatrix(state.latest, matrix) ? stable : changes)
            .fetch_add(1, std::memory_order_relaxed);
    }
    state.entity = entity;
    state.latest = matrix;
    state.serial = g_fixedSerial.load(std::memory_order_relaxed);
    state.hasLatest = true;
}

void SampleTrackedEntityAtRender(
    TracedEntityState& state,
    std::atomic<std::uint64_t>& samples,
    std::atomic<std::uint64_t>& changes,
    std::atomic<std::uint64_t>& repeats) {
    if (!state.hasLatest) {
        return;
    }
    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    if (serial < state.serial || serial - state.serial > 1) {
        state.hasRendered = false;
        return;
    }
    samples.fetch_add(1, std::memory_order_relaxed);
    if (state.hasRendered) {
        (hands_trace::SameMatrix(state.latest, state.rendered) ? repeats : changes)
            .fetch_add(1, std::memory_order_relaxed);
    }
    state.rendered = state.latest;
    state.hasRendered = true;
}

void RecordAttachmentEntity(void* const entity,
                            void* const meshEntity,
                            const hands_trace::Matrix& matrix,
                            const float cameraDistance) {
    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    TracedEntityState* available = nullptr;
    for (std::size_t index = 0; index < g_attachmentStates.size(); ++index) {
        auto& state = g_attachmentStates[index];
        if (state.hasLatest && state.entity == entity) {
            (hands_trace::SameMatrix(state.latest, matrix)
                 ? g_attachmentStable
                 : g_attachmentChanges)
                .fetch_add(1, std::memory_order_relaxed);
            state.latest = matrix;
            state.meshEntity = meshEntity;
            state.cameraDistance = cameraDistance;
            state.serial = serial;
            ++state.intervalWrites;
            return;
        }
        if (available == nullptr &&
            (!state.hasLatest || serial < state.serial ||
             serial - state.serial > 1)) {
            available = &state;
        }
    }
    if (available == nullptr) {
        g_attachmentTrackOverflows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    *available = {};
    available->entity = entity;
    available->meshEntity = meshEntity;
    available->latest = matrix;
    available->cameraDistance = cameraDistance;
    available->serial = serial;
    available->intervalWrites = 1;
    available->hasLatest = true;
    g_attachmentIdentityChanges.fetch_add(1, std::memory_order_relaxed);
}

void RecordSetMatrixMeshEntity(void* const entity,
                               void* const meshEntity,
                               const hands_trace::Matrix& matrix,
                               const float cameraDistance) {
    if (meshEntity == nullptr) {
        return;
    }
    g_setMatrixMeshWrites.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    const std::uint64_t endpointSerial = serial + 1;
    TracedEntityState* available = nullptr;
    for (auto& state : g_setMatrixMeshStates) {
        if (state.hasLatest && state.entity == entity) {
            (hands_trace::SameMatrix(state.latest, matrix)
                 ? g_setMatrixMeshStable
                 : g_setMatrixMeshChanges)
                .fetch_add(1, std::memory_order_relaxed);
            if (endpointSerial > state.endpointSerial) {
                if (endpointSerial - state.endpointSerial == 1) {
                    state.previous = state.latest;
                    state.previousEndpointSerial = state.endpointSerial;
                    state.hasPrevious = true;
                } else {
                    state.hasPrevious = false;
                }
            } else if (endpointSerial < state.endpointSerial) {
                state.hasPrevious = false;
            }
            state.meshEntity = meshEntity;
            state.latest = matrix;
            state.cameraDistance = cameraDistance;
            state.serial = serial;
            state.endpointSerial = endpointSerial;
            ++state.intervalWrites;
            return;
        }
        if (available == nullptr &&
            (!state.hasLatest || serial < state.serial ||
             serial - state.serial > 1)) {
            available = &state;
        }
    }
    if (available == nullptr) {
        g_setMatrixMeshTrackOverflows.fetch_add(1,
                                                std::memory_order_relaxed);
        return;
    }
    *available = {};
    available->entity = entity;
    available->meshEntity = meshEntity;
    available->latest = matrix;
    available->cameraDistance = cameraDistance;
    available->serial = serial;
    available->endpointSerial = endpointSerial;
    available->intervalWrites = 1;
    available->hasLatest = true;
    g_setMatrixMeshIdentityChanges.fetch_add(1, std::memory_order_relaxed);
}

void SampleAttachmentsAtRender() {
    for (auto& state : g_attachmentStates) {
        SampleTrackedEntityAtRender(
            state, g_attachmentRenderSamples, g_attachmentRenderChanges,
            g_attachmentRenderRepeats);
    }
}

bool IsFresh(const TracedEntityState& state,
             const std::uint64_t serial) noexcept {
    return state.hasLatest && serial >= state.serial &&
           serial - state.serial <= 1;
}

std::uint32_t DistanceMillis(const float distance) noexcept {
    return static_cast<std::uint32_t>(std::clamp(
        std::lround(static_cast<double>(distance) * 1000.0), 0L, 100000L));
}

void PrepareRenderOwners() noexcept {
    g_renderOwnerCount = 0;
    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    const auto append = [serial](TracedEntityState& state,
                                 const RenderOwnerKind kind) {
        if (!IsFresh(state, serial) || state.meshEntity == nullptr ||
            g_renderOwnerCount >= g_renderOwners.size()) {
            return;
        }
        const std::uintptr_t owner =
            reinterpret_cast<std::uintptr_t>(state.meshEntity);
        for (std::size_t index = 0; index < g_renderOwnerCount; ++index) {
            if (g_renderOwners[index].owner == owner) {
                return;
            }
        }
        g_renderOwners[g_renderOwnerCount++] = {
            owner,
            reinterpret_cast<std::uintptr_t>(state.entity),
            DistanceMillis(state.cameraDistance),
            &state,
            kind,
        };
    };
    for (auto& state : g_setMatrixMeshStates) {
        append(state, RenderOwnerKind::setMatrix);
    }
    for (auto& state : g_attachmentStates) {
        append(state, RenderOwnerKind::attachment);
    }
}

void RecordRelativeModelMatrices(TracedEntityState& state,
                                 const hands_trace::Matrix& model) noexcept {
    hands_trace::Matrix inverseRoot{};
    if (!hands_trace::InvertAffine(state.latest, inverseRoot)) {
        ++state.intervalRelativeMatrixFailures;
        return;
    }
    constexpr float stableTolerance = 1.0e-4F;
    const hands_trace::Matrix inverseRootTimesModel =
        hands_trace::Multiply(inverseRoot, model);
    if (state.hasInverseRootTimesModel) {
        const float delta = hands_trace::MaximumAbsoluteDifference(
            state.inverseRootTimesModel, inverseRootTimesModel);
        state.intervalInverseRootTimesModelMaxDelta =
            std::max(state.intervalInverseRootTimesModelMaxDelta, delta);
        if (delta <= stableTolerance) {
            ++state.intervalInverseRootTimesModelStable;
        } else {
            ++state.intervalInverseRootTimesModelChanged;
        }
    }
    state.inverseRootTimesModel = inverseRootTimesModel;
    state.hasInverseRootTimesModel = true;

    const hands_trace::Matrix modelTimesInverseRoot =
        hands_trace::Multiply(model, inverseRoot);
    if (state.hasModelTimesInverseRoot) {
        const float delta = hands_trace::MaximumAbsoluteDifference(
            state.modelTimesInverseRoot, modelTimesInverseRoot);
        state.intervalModelTimesInverseRootMaxDelta =
            std::max(state.intervalModelTimesInverseRootMaxDelta, delta);
        if (delta <= stableTolerance) {
            ++state.intervalModelTimesInverseRootStable;
        } else {
            ++state.intervalModelTimesInverseRootChanged;
        }
    }
    state.modelTimesInverseRoot = modelTimesInverseRoot;
    state.hasModelTimesInverseRoot = true;
}

// A substituted model matrix must stay valid and unchanged for as long as
// the renderer may hold the pointer within the frame, so each live submesh
// gets its own slot for the duration of one RenderAll.
struct PresentedModelMatrix {
    void* subMesh = nullptr;
    std::uint64_t renderSerial = 0;
    alignas(16) hands_trace::Matrix matrix{};
};

constexpr std::size_t kPresentedModelMatrixCapacity = 64;
std::array<PresentedModelMatrix, kPresentedModelMatrixCapacity>
    g_presentedModelMatrices{};

hands_trace::Matrix* AcquirePresentedModelMatrix(
    void* const subMesh) noexcept {
    PresentedModelMatrix* available = nullptr;
    for (auto& entry : g_presentedModelMatrices) {
        if (entry.renderSerial == g_handsPoseRenderSerial) {
            if (entry.subMesh == subMesh) {
                return &entry.matrix;
            }
        } else if (available == nullptr) {
            available = &entry;
        }
    }
    if (available == nullptr) {
        return nullptr;
    }
    available->subMesh = subMesh;
    available->renderSerial = g_handsPoseRenderSerial;
    return &available->matrix;
}

enum class HandsInterpolationResult : std::uint8_t {
    applied,
    unavailable,
    invalid,
};

HandsInterpolationResult InterpolateHandsModelMatrix(
    const TracedEntityState& state,
    const hands_trace::Matrix& currentModel,
    hands_trace::Matrix& result) noexcept {
    result = {};
    if (!g_hasActiveHandsClockSample ||
        g_activeHandsClockSample.kind != clock::SampleKind::interpolated ||
        !state.hasPrevious ||
        state.previousEndpointSerial !=
            g_activeHandsClockSample.olderSerial ||
        state.endpointSerial != g_activeHandsClockSample.newerSerial) {
        return HandsInterpolationResult::unavailable;
    }
    hands_trace::Matrix interpolatedRoot{};
    if (!world::InterpolateScaledRigid(
            state.previous, state.latest, g_activeHandsClockSample.alpha,
            interpolatedRoot)) {
        return HandsInterpolationResult::invalid;
    }
    hands_trace::Matrix inverseCurrentRoot{};
    if (!hands_trace::InvertAffine(state.latest, inverseCurrentRoot)) {
        return HandsInterpolationResult::invalid;
    }
    const hands_trace::Matrix local =
        hands_trace::Multiply(inverseCurrentRoot, currentModel);
    result = hands_trace::Multiply(interpolatedRoot, local);
    return hands_trace::IsFiniteAffine(result)
        ? HandsInterpolationResult::applied
        : HandsInterpolationResult::invalid;
}

bool StageEntityIdentityProbe(void* const entity) {
    if (entity == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < g_handsProbeTransaction.count; ++i) {
        if (g_handsProbeTransaction.entries[i].entity == entity) {
            return true;
        }
    }
    for (std::size_t i = 0; i < g_handsProbeRejectedCount; ++i) {
        if (g_handsProbeRejectedEntities[i] == entity) {
            g_handsProbeExcludedEntities.fetch_add(1,
                                                   std::memory_order_relaxed);
            return true;
        }
    }
    const auto rejectEntity = [entity]() {
        if (g_handsProbeRejectedCount < g_handsProbeRejectedEntities.size()) {
            g_handsProbeRejectedEntities[g_handsProbeRejectedCount++] = entity;
        }
    };
    if (g_handsProbeTransaction.count >=
        g_handsProbeTransaction.entries.size()) {
        g_handsProbeWriteFailures.fetch_add(1, std::memory_order_relaxed);
        g_handsProbeLatestFailureEntity.store(
            reinterpret_cast<std::uintptr_t>(entity),
            std::memory_order_relaxed);
        rejectEntity();
        return false;
    }
    hands_trace::Matrix original{};
    if (!hands_trace::ReadEntityMatrix(entity, original)) {
        g_handsProbeInitialReadFailures.fetch_add(1,
                                                  std::memory_order_relaxed);
        g_handsProbeLatestFailureEntity.store(
            reinterpret_cast<std::uintptr_t>(entity),
            std::memory_order_relaxed);
        g_handsProbeLatestFailureDistanceMillis.store(0,
                                                       std::memory_order_relaxed);
        rejectEntity();
        return false;
    }
    camera_trace::Sample camera{};
    const bool hasCamera = camera_trace::ReadCurrentListenerCamera(
        g_sceneGlobalPointer, camera) == camera_trace::SampleStatus::ok;
    const float distance = hasCamera
        ? hands_trace::DistanceFrom(original, camera.position)
        : 0.0F;
    const auto distanceMillis = static_cast<std::uint32_t>(std::clamp(
        std::lround(static_cast<double>(distance) * 1000.0), 0L, 100000L));
    if (!hands_trace::WriteEntityMatrix(entity, original)) {
        g_handsProbeWriteFailures.fetch_add(1, std::memory_order_relaxed);
        g_handsProbeLatestFailureEntity.store(
            reinterpret_cast<std::uintptr_t>(entity),
            std::memory_order_relaxed);
        g_handsProbeLatestFailureDistanceMillis.store(
            distanceMillis, std::memory_order_relaxed);
        rejectEntity();
        return false;
    }
    hands_trace::Matrix verified{};
    if (!hands_trace::ReadEntityMatrix(entity, verified)) {
        g_handsProbeVerifyReadFailures.fetch_add(1,
                                                 std::memory_order_relaxed);
        g_handsProbeLatestFailureEntity.store(
            reinterpret_cast<std::uintptr_t>(entity),
            std::memory_order_relaxed);
        g_handsProbeLatestFailureDistanceMillis.store(
            distanceMillis, std::memory_order_relaxed);
        (void)hands_trace::WriteEntityMatrix(entity, original);
        rejectEntity();
        return false;
    }
    if (!hands_trace::SameMatrix(original, verified)) {
        g_handsProbeVerifyMismatches.fetch_add(1,
                                              std::memory_order_relaxed);
        g_handsProbeLatestFailureEntity.store(
            reinterpret_cast<std::uintptr_t>(entity),
            std::memory_order_relaxed);
        g_handsProbeLatestFailureDistanceMillis.store(
            distanceMillis, std::memory_order_relaxed);
        (void)hands_trace::WriteEntityMatrix(entity, original);
        rejectEntity();
        return false;
    }
    auto& entry =
        g_handsProbeTransaction.entries[g_handsProbeTransaction.count++];
    entry.entity = entity;
    entry.original = original;
    g_handsProbeStagedEntities.fetch_add(1, std::memory_order_relaxed);
    g_handsProbeLatestStagedEntity.store(
        reinterpret_cast<std::uintptr_t>(entity), std::memory_order_relaxed);
    g_handsProbeLatestStagedDistanceMillis.store(distanceMillis,
                                                 std::memory_order_relaxed);
    return true;
}

void BeginHandsIdentityProbe() {
    if (!g_handsRestoreProbe) {
        return;
    }
    if (g_handsProbeTransaction.active) {
        g_handsProbeFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_handsProbeTransaction.count = 0;
    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    bool failed = false;
    if (IsFresh(g_handsRootState, serial) &&
        !StageEntityIdentityProbe(g_handsRootState.entity)) {
        failed = true;
    }
    for (const auto& state : g_attachmentStates) {
        if (IsFresh(state, serial) && !StageEntityIdentityProbe(state.entity)) {
            failed = true;
        }
    }
    if (failed) {
        g_handsProbeFailures.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_handsProbeTransaction.count == 0) {
        g_handsProbeSkippedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_handsProbeTransaction.active = true;
    g_handsProbeStagedFrames.fetch_add(1, std::memory_order_relaxed);
}

void RestoreHandsIdentityProbe() {
    if (!g_handsProbeTransaction.active) {
        return;
    }
    bool restoredAll = true;
    for (std::size_t i = g_handsProbeTransaction.count; i > 0; --i) {
        const auto& entry = g_handsProbeTransaction.entries[i - 1];
        hands_trace::Matrix verified{};
        if (!hands_trace::WriteEntityMatrix(entry.entity, entry.original) ||
            !hands_trace::ReadEntityMatrix(entry.entity, verified) ||
            !hands_trace::SameMatrix(entry.original, verified)) {
            restoredAll = false;
            continue;
        }
        g_handsProbeRestoredEntities.fetch_add(1,
                                               std::memory_order_relaxed);
    }
    if (restoredAll) {
        g_handsProbeRestoredFrames.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_handsProbeFailures.fetch_add(1, std::memory_order_relaxed);
    }
    g_handsProbeTransaction.active = false;
    g_handsProbeTransaction.count = 0;
}

void ReportCadence(const std::int64_t elapsedTicks) {
    const cadence::Snapshot snapshot = g_presentCadence.TakeSnapshot();
    const std::uint64_t fixedUpdates =
        g_fixedUpdates.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t completedFixedUpdates =
        g_completedFixedUpdates.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t variableUpdates =
        g_variableUpdates.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t drawFrames =
        g_drawFrames.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t endFrames =
        g_endFrames.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t phaseSamples =
        g_phaseSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t invalidPhaseSamples =
        g_invalidPhaseSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t phaseRegressions =
        g_phaseRegressions.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t phaseHolds =
        g_phaseHolds.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t nextFieldUnderRange =
        g_nextFieldUnderRange.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t nextFieldOverRange =
        g_nextFieldOverRange.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t completedFieldValid =
        g_completedFieldValid.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t completedFieldUnderRange =
        g_completedFieldUnderRange.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t completedFieldOverRange =
        g_completedFieldOverRange.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t threadMismatches =
        g_threadMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t limiterWaits =
        g_limiterWaits.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t limiterPhaseResets =
        g_limiterPhaseResets.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraCompletedSamples =
        g_cameraCompletedSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraCompletedChanges =
        g_cameraCompletedChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraCompletedStable =
        g_cameraCompletedStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraVariableSamples =
        g_cameraVariableSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraVariableChanges =
        g_cameraVariableChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraVariableStable =
        g_cameraVariableStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraDrawSamples =
        g_cameraDrawSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraDrawChanges =
        g_cameraDrawChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraDrawStable =
        g_cameraDrawStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraPresentSamples =
        g_cameraPresentSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraPresentChanges =
        g_cameraPresentChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraPresentRepeats =
        g_cameraPresentRepeats.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraIdentityMismatches =
        g_cameraIdentityMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraStateMismatches =
        g_cameraStateMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraVariableIdentityMismatches =
        g_cameraVariableIdentityMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraVariableStateMismatches =
        g_cameraVariableStateMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraDrawIdentityMismatches =
        g_cameraDrawIdentityMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraDrawStateMismatches =
        g_cameraDrawStateMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInvalidSamples =
        g_cameraInvalidSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t listenerIdentityChanges =
        g_listenerIdentityChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraIdentityChanges =
        g_cameraIdentityChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraProbeStaged =
        g_cameraProbeStaged.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraProbeSkipped =
        g_cameraProbeSkipped.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraProbeRestored =
        g_cameraProbeRestored.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraProbeIdentityChanges =
        g_cameraProbeIdentityChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraProbeFailures =
        g_cameraProbeFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationCaptures =
        g_cameraInterpolationCaptures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationApplied =
        g_cameraInterpolationApplied.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationRestored =
        g_cameraInterpolationRestored.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationSkipped =
        g_cameraInterpolationSkipped.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationResets =
        g_cameraInterpolationResets.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t cameraInterpolationFailures =
        g_cameraInterpolationFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixCalls =
        g_entitySetMatrixCalls.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixFinite =
        g_entitySetMatrixFinite.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixCameraReady =
        g_entitySetMatrixCameraReady.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixNearCamera =
        g_entitySetMatrixNearCamera.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixWithinHalfMeter =
        g_entitySetMatrixWithinHalfMeter.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixQuarterScale =
        g_entitySetMatrixQuarterScale.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixUnitScale =
        g_entitySetMatrixUnitScale.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t entitySetMatrixOtherScale =
        g_entitySetMatrixOtherScale.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootWrites =
        g_handsRootWrites.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootChanges =
        g_handsRootChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootStable =
        g_handsRootStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootIdentityChanges =
        g_handsRootIdentityChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootRenderSamples =
        g_handsRootRenderSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootRenderChanges =
        g_handsRootRenderChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsRootRenderRepeats =
        g_handsRootRenderRepeats.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentCalls =
        g_attachmentCalls.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentNearCamera =
        g_attachmentNearCamera.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentChanges =
        g_attachmentChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentStable =
        g_attachmentStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentIdentityChanges =
        g_attachmentIdentityChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentRenderSamples =
        g_attachmentRenderSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentRenderChanges =
        g_attachmentRenderChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentRenderRepeats =
        g_attachmentRenderRepeats.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t attachmentTrackOverflows =
        g_attachmentTrackOverflows.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsTraceInvalid =
        g_handsTraceInvalid.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t setMatrixMeshWrites =
        g_setMatrixMeshWrites.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t setMatrixMeshChanges =
        g_setMatrixMeshChanges.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t setMatrixMeshStable =
        g_setMatrixMeshStable.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t setMatrixMeshIdentityChanges =
        g_setMatrixMeshIdentityChanges.exchange(0,
                                                std::memory_order_acq_rel);
    const std::uint64_t setMatrixMeshTrackOverflows =
        g_setMatrixMeshTrackOverflows.exchange(0,
                                               std::memory_order_acq_rel);
    const std::uint64_t subMeshModelSetMatrixMatches =
        g_subMeshModelSetMatrixMatches.exchange(0,
                                                std::memory_order_acq_rel);
    const std::uint64_t subMeshModelAttachmentMatches =
        g_subMeshModelAttachmentMatches.exchange(0,
                                                 std::memory_order_acq_rel);
    const std::uint64_t handsRenderCopyReturns =
        g_handsRenderCopyReturns.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsInterpolationApplied =
        g_handsInterpolationApplied.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsInterpolationSkipped =
        g_handsInterpolationSkipped.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsInterpolationFailures =
        g_handsInterpolationFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseSkinCalls =
        g_handsPoseSkinCalls.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseOwnerMatches =
        g_handsPoseOwnerMatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPosePaletteValid =
        g_handsPosePaletteValid.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPosePaletteEmpty =
        g_handsPosePaletteEmpty.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPosePaletteInvalid =
        g_handsPosePaletteInvalid.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseNearSamples =
        g_handsPoseNearSamples.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseTrackOverflows =
        g_handsPoseTrackOverflows.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererCalls =
        g_handsPoseRendererCalls.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererOwnerMatches =
        g_handsPoseRendererOwnerMatches.exchange(0,
                                                 std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererPaletteValid =
        g_handsPoseRendererPaletteValid.exchange(0,
                                                 std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererPaletteInvalid =
        g_handsPoseRendererPaletteInvalid.exchange(0,
                                                   std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererPointerMismatches =
        g_handsPoseRendererPointerMismatches.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererCopyReturns =
        g_handsPoseRendererCopyReturns.exchange(0,
                                                std::memory_order_acq_rel);
    const std::uint64_t handsPoseRendererCopyFailures =
        g_handsPoseRendererCopyFailures.exchange(0,
                                                 std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeArmed =
        g_handsPoseProbeArmed.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeUpdaterMatches =
        g_handsPoseProbeUpdaterMatches.exchange(0,
                                                std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeStaged =
        g_handsPoseProbeStaged.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeRestored =
        g_handsPoseProbeRestored.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeFailures =
        g_handsPoseProbeFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseProbeVerifyMismatches =
        g_handsPoseProbeVerifyMismatches.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseInterpolationApplied =
        g_handsPoseInterpolationApplied.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseInterpolationReturns =
        g_handsPoseInterpolationReturns.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseInterpolationSkipped =
        g_handsPoseInterpolationSkipped.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsPoseInterpolationFailures =
        g_handsPoseInterpolationFailures.exchange(
            0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeStagedFrames =
        g_handsProbeStagedFrames.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeStagedEntities =
        g_handsProbeStagedEntities.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeSkippedFrames =
        g_handsProbeSkippedFrames.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeRestoredFrames =
        g_handsProbeRestoredFrames.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeRestoredEntities =
        g_handsProbeRestoredEntities.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeFailures =
        g_handsProbeFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeInitialReadFailures =
        g_handsProbeInitialReadFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeWriteFailures =
        g_handsProbeWriteFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeVerifyReadFailures =
        g_handsProbeVerifyReadFailures.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeVerifyMismatches =
        g_handsProbeVerifyMismatches.exchange(0, std::memory_order_acq_rel);
    const std::uint64_t handsProbeExcludedEntities =
        g_handsProbeExcludedEntities.exchange(0, std::memory_order_acq_rel);
    const double elapsedSeconds = static_cast<double>(elapsedTicks) /
                                  static_cast<double>(g_qpcFrequency);
    const double presentRate = elapsedSeconds > 0.0
        ? static_cast<double>(snapshot.samples) / elapsedSeconds
        : 0.0;
    const double updateRate = elapsedSeconds > 0.0
        ? static_cast<double>(fixedUpdates) / elapsedSeconds
        : 0.0;
    const auto rate = [elapsedSeconds](const std::uint64_t count) {
        return elapsedSeconds > 0.0
            ? static_cast<double>(count) / elapsedSeconds
            : 0.0;
    };
    std::wstring limitFps = L"unavailable";
    if (g_engineGlobalPointer != 0) {
        void* engine = nullptr;
        std::memcpy(&engine, reinterpret_cast<const void*>(g_engineGlobalPointer),
                    sizeof(engine));
        if (engine != nullptr) {
            std::uint8_t enabled = 0;
            std::memcpy(&enabled,
                        static_cast<const std::uint8_t*>(engine) + 0x88,
                        sizeof(enabled));
            limitFps = enabled == 0 ? L"false" : L"true";
        }
    }
    std::wostringstream line;
    line << std::fixed << std::setprecision(3)
         << L"Telemetry: presentHz=" << presentRate
         << L", fixedUpdateHz=" << updateRate
         << L", completedUpdateHz=" << rate(completedFixedUpdates)
         << L", variableUpdateHz=" << rate(variableUpdates)
         << L", drawHz=" << rate(drawFrames)
         << L", endFrameHz=" << rate(endFrames)
         << L", configuredUpdateHz="
         << g_configuredUpdateRate.load(std::memory_order_relaxed)
         << L", limitFPS=" << limitFps
         << L", latestUpdatesThisFrame="
         << g_latestUpdatesThisFrame.load(std::memory_order_relaxed)
         << L", acceptedFixedUpdates=" << fixedUpdates
         << L", presentSamples=" << snapshot.samples
         << L", mean=" << snapshot.meanMilliseconds << L" ms"
         << L", p50=" << snapshot.p50Milliseconds << L" ms"
         << L", p95=" << snapshot.p95Milliseconds << L" ms"
         << L", p99=" << snapshot.p99Milliseconds << L" ms"
         << L", max=" << snapshot.maxMilliseconds << L" ms"
         << L", overflow=" << snapshot.overflow
         << L", longGaps=" << snapshot.longGaps
         << L", invalid=" << snapshot.invalid;
    line << L", schedulerAlpha="
         << static_cast<double>(
                g_latestAlphaPartsPerMillion.load(std::memory_order_relaxed)) /
                1'000'000.0
         << L", phaseSamples=" << phaseSamples
         << L", invalidPhase=" << invalidPhaseSamples
         << L", phaseRegressions=" << phaseRegressions
         << L", phaseHolds=" << phaseHolds
         << L", nextUnder=" << nextFieldUnderRange
         << L", nextOver=" << nextFieldOverRange
         << L", completedCandidateValid=" << completedFieldValid
         << L", completedCandidateUnder=" << completedFieldUnderRange
         << L", completedCandidateOver=" << completedFieldOverRange
         << L", threadMismatches=" << threadMismatches;
    if (g_presentationLimiter) {
        line << L", limiterWaits=" << limiterWaits
             << L", limiterPhaseResets=" << limiterPhaseResets;
    }
    if (g_evenFramePacing) {
        line << L", paceFramesPerTick=" << g_evenPacer.FramesPerTick()
             << L", pacedFrames="
             << g_pacedFrames.exchange(0, std::memory_order_acq_rel)
             << L", paceLateFrames="
             << g_paceLateFrames.exchange(0, std::memory_order_acq_rel)
             << L", paceFallbackFrames="
             << g_paceFallbackFrames.exchange(0, std::memory_order_acq_rel)
             << L", paceTickMisses="
             << g_paceTickMisses.exchange(0, std::memory_order_acq_rel)
             << L", paceRenderLeadMs="
             << static_cast<double>(g_evenPacer.RenderLead()) * 1000.0 /
                    static_cast<double>(g_qpcFrequency)
             << L", paceTickFrameMs="
             << static_cast<double>(g_evenPacer.TickFrameLength()) * 1000.0 /
                    static_cast<double>(g_qpcFrequency)
             << L", pacePlainFrameMs="
             << static_cast<double>(g_evenPacer.PlainFrameLength()) * 1000.0 /
                    static_cast<double>(g_qpcFrequency);
    }
    if (g_motionTraceEnabled) {
        line << L", cameraCompletedSamples=" << cameraCompletedSamples
             << L", cameraCompletedChanges=" << cameraCompletedChanges
             << L", cameraCompletedStable=" << cameraCompletedStable
             << L", cameraVariableSamples=" << cameraVariableSamples
             << L", cameraVariableChanges=" << cameraVariableChanges
             << L", cameraVariableStable=" << cameraVariableStable
             << L", cameraDrawSamples=" << cameraDrawSamples
             << L", cameraDrawChanges=" << cameraDrawChanges
             << L", cameraDrawStable=" << cameraDrawStable
             << L", cameraPresentSamples=" << cameraPresentSamples
             << L", cameraPresentChanges=" << cameraPresentChanges
             << L", cameraPresentRepeats=" << cameraPresentRepeats
             << L", cameraIdentityMismatches=" << cameraIdentityMismatches
             << L", cameraStateMismatches=" << cameraStateMismatches
             << L", cameraVariableIdentityMismatches="
             << cameraVariableIdentityMismatches
             << L", cameraVariableStateMismatches="
             << cameraVariableStateMismatches
             << L", cameraDrawIdentityMismatches="
             << cameraDrawIdentityMismatches
             << L", cameraDrawStateMismatches="
             << cameraDrawStateMismatches
             << L", cameraInvalid=" << cameraInvalidSamples
             << L", listenerChanges=" << listenerIdentityChanges
             << L", cameraChanges=" << cameraIdentityChanges
             << L", listener=0x" << std::hex
             << g_latestListenerPointer.load(std::memory_order_relaxed)
             << L", camera=0x"
             << g_latestCameraPointer.load(std::memory_order_relaxed)
             << std::dec;
    }
    if (g_cameraRestoreProbe) {
        line << L", cameraProbeStaged=" << cameraProbeStaged
             << L", cameraProbeSkipped=" << cameraProbeSkipped
             << L", cameraProbeRestored=" << cameraProbeRestored
             << L", cameraProbeIdentityChanges="
             << cameraProbeIdentityChanges
             << L", cameraProbeFailures=" << cameraProbeFailures;
    }
    if (g_cameraInterpolation) {
        line << L", cameraInterpolationCaptures="
             << cameraInterpolationCaptures
             << L", cameraInterpolationApplied=" << cameraInterpolationApplied
             << L", cameraInterpolationRestored="
             << cameraInterpolationRestored
             << L", cameraInterpolationSkipped="
             << cameraInterpolationSkipped
             << L", cameraInterpolationResets=" << cameraInterpolationResets
             << L", cameraInterpolationFailures="
             << cameraInterpolationFailures;
    }
    if (g_handsTraceEnabled) {
        line << L", entitySetMatrixCalls=" << entitySetMatrixCalls
             << L", entitySetMatrixFinite=" << entitySetMatrixFinite
             << L", entitySetMatrixCameraReady="
             << entitySetMatrixCameraReady
             << L", entitySetMatrixNearCamera="
             << entitySetMatrixNearCamera
             << L", entitySetMatrixWithinHalfMeter="
             << entitySetMatrixWithinHalfMeter
             << L", entitySetMatrixQuarterScale="
             << entitySetMatrixQuarterScale
             << L", entitySetMatrixUnitScale=" << entitySetMatrixUnitScale
             << L", entitySetMatrixOtherScale=" << entitySetMatrixOtherScale
             << L", latestSetMatrixScale="
             << static_cast<double>(g_latestSetMatrixScaleXMillis.load(
                    std::memory_order_relaxed)) / 1000.0
             << L"/"
             << static_cast<double>(g_latestSetMatrixScaleYMillis.load(
                    std::memory_order_relaxed)) / 1000.0
             << L"/"
             << static_cast<double>(g_latestSetMatrixScaleZMillis.load(
                    std::memory_order_relaxed)) / 1000.0
             << L", latestSetMatrixDistance="
             << static_cast<double>(g_latestSetMatrixDistanceMillis.load(
                    std::memory_order_relaxed)) / 1000.0
             << L", latestSetMatrixEntity=0x" << std::hex
             << g_latestSetMatrixEntity.load(std::memory_order_relaxed)
             << std::dec
             << L", handsRootWrites=" << handsRootWrites
             << L", handsRootChanges=" << handsRootChanges
             << L", handsRootStable=" << handsRootStable
             << L", handsRootIdentityChanges=" << handsRootIdentityChanges
             << L", handsRootRenderSamples=" << handsRootRenderSamples
             << L", handsRootRenderChanges=" << handsRootRenderChanges
             << L", handsRootRenderRepeats=" << handsRootRenderRepeats
             << L", attachmentCalls=" << attachmentCalls
             << L", attachmentNearCamera=" << attachmentNearCamera
             << L", attachmentChanges=" << attachmentChanges
             << L", attachmentStable=" << attachmentStable
             << L", attachmentIdentityChanges=" << attachmentIdentityChanges
             << L", attachmentRenderSamples=" << attachmentRenderSamples
             << L", attachmentRenderChanges=" << attachmentRenderChanges
             << L", attachmentRenderRepeats=" << attachmentRenderRepeats
             << L", attachmentTrackOverflows=" << attachmentTrackOverflows
             << L", handsTraceInvalid=" << handsTraceInvalid
             << L", setMatrixMeshWrites=" << setMatrixMeshWrites
             << L", setMatrixMeshChanges=" << setMatrixMeshChanges
             << L", setMatrixMeshStable=" << setMatrixMeshStable
             << L", setMatrixMeshIdentityChanges="
             << setMatrixMeshIdentityChanges
             << L", setMatrixMeshTrackOverflows="
             << setMatrixMeshTrackOverflows
             << L", subMeshModelSetMatrixMatches="
             << subMeshModelSetMatrixMatches
             << L", subMeshModelAttachmentMatches="
             << subMeshModelAttachmentMatches
             << L", handsRenderCopyReturns=" << handsRenderCopyReturns
             << L", handsInterpolationApplied="
             << handsInterpolationApplied
             << L", handsInterpolationSkipped="
             << handsInterpolationSkipped
             << L", handsInterpolationFailures="
             << handsInterpolationFailures
             << L", handsPoseSkinCalls=" << handsPoseSkinCalls
             << L", handsPoseOwnerMatches=" << handsPoseOwnerMatches
             << L", handsPosePaletteValid=" << handsPosePaletteValid
             << L", handsPosePaletteEmpty=" << handsPosePaletteEmpty
             << L", handsPosePaletteInvalid=" << handsPosePaletteInvalid
             << L", handsPoseNearSamples=" << handsPoseNearSamples
             << L", handsPoseTrackOverflows=" << handsPoseTrackOverflows
             << L", handsPoseRendererCalls=" << handsPoseRendererCalls
             << L", handsPoseRendererOwnerMatches="
             << handsPoseRendererOwnerMatches
             << L", handsPoseRendererPaletteValid="
             << handsPoseRendererPaletteValid
             << L", handsPoseRendererPaletteInvalid="
             << handsPoseRendererPaletteInvalid
             << L", handsPoseRendererPointerMismatches="
             << handsPoseRendererPointerMismatches
             << L", handsPoseRendererCopyReturns="
             << handsPoseRendererCopyReturns
             << L", handsPoseRendererCopyFailures="
             << handsPoseRendererCopyFailures
             << L", handsPoseProbeArmed=" << handsPoseProbeArmed
             << L", handsPoseProbeUpdaterMatches="
             << handsPoseProbeUpdaterMatches
             << L", handsPoseProbeStaged=" << handsPoseProbeStaged
             << L", handsPoseProbeRestored=" << handsPoseProbeRestored
             << L", handsPoseProbeFailures=" << handsPoseProbeFailures
             << L", handsPoseProbeVerifyMismatches="
             << handsPoseProbeVerifyMismatches
             << L", handsPoseInterpolationApplied="
             << handsPoseInterpolationApplied
             << L", handsPoseInterpolationReturns="
             << handsPoseInterpolationReturns
             << L", handsPoseInterpolationSkipped="
             << handsPoseInterpolationSkipped
             << L", handsPoseInterpolationFailures="
             << handsPoseInterpolationFailures
             << L", latestSetMatrixRenderOwner=0x" << std::hex
             << g_latestSetMatrixRenderOwner.load(std::memory_order_relaxed)
             << L", latestSetMatrixRenderEntity=0x"
             << g_latestSetMatrixRenderEntity.load(std::memory_order_relaxed)
             << L", latestSetMatrixRenderDistance=" << std::dec
             << static_cast<double>(
                    g_latestSetMatrixRenderDistanceMillis.load(
                        std::memory_order_relaxed)) / 1000.0
             << L", handsRootMesh=0x" << std::hex
             << g_handsRootMeshEntity
             << L", handsRoot=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(g_handsRootState.entity)
             << L", attachment=0x"
             << g_latestAttachmentPointer.load(std::memory_order_relaxed)
             << std::dec;
        line << L", setMatrixMeshSlots=[";
        bool firstSetMatrixMeshSlot = true;
        for (auto& state : g_setMatrixMeshStates) {
            if (state.intervalWrites == 0 &&
                state.intervalRenderMatches == 0) {
                continue;
            }
            if (!firstSetMatrixMeshSlot) {
                line << L";";
            }
            firstSetMatrixMeshSlot = false;
            line << L"entity=0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(state.entity)
                 << L"/owner=0x"
                 << reinterpret_cast<std::uintptr_t>(state.meshEntity)
                 << std::dec
                 << L"/distance=" << state.cameraDistance
                 << L"/writes=" << state.intervalWrites
                 << L"/render=" << state.intervalRenderMatches
                 << L"/exact=" << state.intervalExactMatrixMatches
                 << L"/different=" << state.intervalDifferentMatrixMatches
                 << L"/invRootModelStable="
                 << state.intervalInverseRootTimesModelStable
                 << L"/invRootModelChanged="
                 << state.intervalInverseRootTimesModelChanged
                 << L"/invRootModelMaxDelta="
                 << state.intervalInverseRootTimesModelMaxDelta
                 << L"/invRootModelT="
                 << state.inverseRootTimesModel.values[3] << L"/"
                 << state.inverseRootTimesModel.values[7] << L"/"
                 << state.inverseRootTimesModel.values[11]
                 << L"/modelInvRootStable="
                 << state.intervalModelTimesInverseRootStable
                 << L"/modelInvRootChanged="
                 << state.intervalModelTimesInverseRootChanged
                 << L"/modelInvRootMaxDelta="
                 << state.intervalModelTimesInverseRootMaxDelta
                 << L"/modelInvRootT="
                 << state.modelTimesInverseRoot.values[3] << L"/"
                 << state.modelTimesInverseRoot.values[7] << L"/"
                 << state.modelTimesInverseRoot.values[11]
                 << L"/relativeFailures="
                 << state.intervalRelativeMatrixFailures;
            state.intervalWrites = 0;
            state.intervalRenderMatches = 0;
            state.intervalExactMatrixMatches = 0;
            state.intervalDifferentMatrixMatches = 0;
            state.intervalInverseRootTimesModelStable = 0;
            state.intervalInverseRootTimesModelChanged = 0;
            state.intervalModelTimesInverseRootStable = 0;
            state.intervalModelTimesInverseRootChanged = 0;
            state.intervalRelativeMatrixFailures = 0;
            state.intervalInverseRootTimesModelMaxDelta = 0.0F;
            state.intervalModelTimesInverseRootMaxDelta = 0.0F;
        }
        line << L"]";
        line << L", handsPoseSlots=[";
        bool firstHandsPoseSlot = true;
        for (auto& state : g_handsPoseStates) {
            if (state.intervalSamples == 0 &&
                state.intervalRendererSamples == 0) {
                continue;
            }
            if (!firstHandsPoseSlot) {
                line << L";";
            }
            firstHandsPoseSlot = false;
            line << L"owner=0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(state.owner)
                 << L"/entity=0x" << state.renderEntity
                 << std::dec
                 << L"/kind="
                 << (state.renderKind == RenderOwnerKind::setMatrix
                         ? L"setMatrix"
                         : L"attachment")
                 << L"/distance=" << state.modelDistance
                 << L"/bones=" << state.boneCount
                 << L"/maxBones=" << state.maximumBoneCount
                 << L"/samples=" << state.intervalSamples
                 << L"/changes=" << state.intervalChanges
                 << L"/repeats=" << state.intervalRepeats
                 << L"/countChanges=" << state.intervalBoneCountChanges
                 << L"/rootOwnerMatches="
                 << state.intervalRenderOwnerMatches
                 << L"/rendererBones=" << state.rendererBoneCount
                 << L"/rendererMaxBones="
                 << state.maximumRendererBoneCount
                 << L"/rendererSamples="
                 << state.intervalRendererSamples
                 << L"/rendererChanges="
                 << state.intervalRendererChanges
                 << L"/rendererRepeats="
                 << state.intervalRendererRepeats;
            state.intervalSamples = 0;
            state.intervalChanges = 0;
            state.intervalRepeats = 0;
            state.intervalBoneCountChanges = 0;
            state.intervalRenderOwnerMatches = 0;
            state.intervalRendererSamples = 0;
            state.intervalRendererChanges = 0;
            state.intervalRendererRepeats = 0;
        }
        line << L"]";
        if (g_handsRestoreProbe) {
            line << L", handsProbeStagedFrames=" << handsProbeStagedFrames
                 << L", handsProbeStagedEntities=" << handsProbeStagedEntities
                 << L", handsProbeSkippedFrames=" << handsProbeSkippedFrames
                 << L", handsProbeRestoredFrames="
                 << handsProbeRestoredFrames
                 << L", handsProbeRestoredEntities="
                 << handsProbeRestoredEntities
                 << L", handsProbeFailures=" << handsProbeFailures
                 << L", handsProbeInitialReadFailures="
                 << handsProbeInitialReadFailures
                 << L", handsProbeWriteFailures=" << handsProbeWriteFailures
                 << L", handsProbeVerifyReadFailures="
                 << handsProbeVerifyReadFailures
                 << L", handsProbeVerifyMismatches="
                 << handsProbeVerifyMismatches
                 << L", handsProbeExcludedEntities="
                 << handsProbeExcludedEntities
                 << L", handsProbeLatestStagedEntity=0x" << std::hex
                 << g_handsProbeLatestStagedEntity.load(
                        std::memory_order_relaxed)
                 << L", handsProbeLatestStagedDistance=" << std::dec
                 << static_cast<double>(
                        g_handsProbeLatestStagedDistanceMillis.load(
                            std::memory_order_relaxed)) / 1000.0
                 << L", handsProbeLatestFailureEntity=0x" << std::hex
                 << g_handsProbeLatestFailureEntity.load(
                        std::memory_order_relaxed)
                 << L", handsProbeLatestFailureDistance=" << std::dec
                 << static_cast<double>(
                        g_handsProbeLatestFailureDistanceMillis.load(
                            std::memory_order_relaxed)) / 1000.0;
        }
    }
    if (g_cameraInterpolation) {
        line << L", cameraStage=[";
        bool first = true;
        for (std::size_t index = 0; index < kCameraStageResultCount; ++index) {
            const std::uint64_t count = g_cameraStageResults[index].exchange(
                0, std::memory_order_acq_rel);
            if (count == 0) {
                continue;
            }
            line << (first ? L"" : L";")
                 << camera::TransactionResultName(
                        static_cast<camera::TransactionResult>(index))
                 << L"=" << count;
            first = false;
        }
        line << L"]";
    }
    if (g_gameplayRateOverride != 0) {
        line << L", gameplayRateOverride=" << g_gameplayRateOverride
             << L", gameplayRateApplications="
             << g_gameplayRateApplications.load(std::memory_order_relaxed);
    }
    if (g_mouseTraceEnabled) {
        const mouse::Report mouseReport = mouse::TakeReport();
        line << L", mouseMotionEvents=" << mouseReport.motionEvents
             << L", mouseMotionFrames=" << mouseReport.motionFrames
             << L", mouseConsumeCalls=" << mouseReport.consumeCalls
             << L", mouseConsumeNonZero=" << mouseReport.consumeNonZero
             << L", mouseObserved=" << mouseReport.observedX << L"/"
             << mouseReport.observedY << L", mouseConsumed="
             << mouseReport.consumedX << L"/" << mouseReport.consumedY
             << L", mouseMaxDrift=" << mouseReport.maximumDrift
             << L", mouseTicks=" << mouseReport.ticks
             << L", mouseTicksWithMotion=" << mouseReport.ticksWithMotion
             << L", mouseTurnsWithoutMotion="
             << mouseReport.turnsWithoutMotion
             << L", mouseScaleReady=" << (mouseReport.scaleReady ? 1 : 0)
             << L", mouseAxesReady=" << (mouseReport.axesReady ? 1 : 0)
             << L", mouseAxisVotes=" << mouseReport.axisVotes[0][0] << L"/"
             << mouseReport.axisVotes[0][1] << L"/"
             << mouseReport.axisVotes[0][2] << L"/"
             << mouseReport.axisVotes[0][3] << L"/"
             << mouseReport.axisVotes[0][4] << L"|"
             << mouseReport.axisVotes[1][0] << L"/"
             << mouseReport.axisVotes[1][1] << L"/"
             << mouseReport.axisVotes[1][2] << L"/"
             << mouseReport.axisVotes[1][3] << L"/"
             << mouseReport.axisVotes[1][4]
             << L", mouseLeadFrames=" << mouseReport.leadFrames
             << L", viewLeadFrames="
             << g_viewLeadFrames.exchange(0, std::memory_order_acq_rel)
             << L", mouseMismatchTicks=" << mouseReport.mismatchTicks
             << L", mouseSuspendedTicks=" << mouseReport.suspendedTicks
             << L", mouseClampedTicks=" << mouseReport.clampedTicks
             << L", mouseScaleChanges=" << mouseReport.scaleChanges
             << L", mouseDetaches=" << mouseReport.detaches
             << std::scientific << std::setprecision(5)
             << L", mouseYawPerCount=" << mouseReport.yawPerCount
             << L", mousePitchPerCount=" << mouseReport.pitchPerCount
             << std::fixed << std::setprecision(4) << L", mousePitchLimits="
             << mouseReport.pitchLow << L"/" << mouseReport.pitchHigh
             << L", mouseMisses=[";
        for (std::size_t index = 0; index < mouseReport.missCount; ++index) {
            const mouse::Report::Miss& miss = mouseReport.misses[index];
            line << (index == 0 ? L"" : L";") << miss.axis << L":"
                 << miss.counts << L":" << miss.angle << L":"
                 << miss.expected;
        }
        line << L"]";
        line.unsetf(std::ios::floatfield);
        line << std::setprecision(6);
    }
    if (g_particleInterpolation) {
        const particle_runtime::Counters particles =
            particle_runtime::TakeCounters();
        line << L", particleUpdates=" << particles.updates
             << L", particleOffMainThread=" << particles.offMainThread
             << L", particleInterpolated=" << particles.interpolated
             << L", particlesMoved=" << particles.particles
             << L", particleIdle=" << particles.idle
             << L", particleUnavailable=" << particles.unavailable
             << L", particleOversize=" << particles.oversize
             << L", particleInvalid=" << particles.invalid
             << L", particleBusy=" << particles.busy
             << L", particleTableResets=" << particles.tableResets
             << L", particleLargestEmitter=" << particles.largestEmitter;
    }
    if (g_guiCursor) {
        const gui_cursor::Report gui = gui_cursor::TakeReport();
        line << L", guiMouseSends=" << gui.sends
             << L", guiSendsMatchingOs=" << gui.sendsMatchingOs
             << L", guiSendsMatchingMotion=" << gui.sendsMatchingMotion
             << L", guiVirtualSends=" << gui.virtualSends
             << L", guiCursorsQueued=" << gui.cursorsQueued
             << L", guiReplays=" << gui.replays
             << L", guiCursorMoved=" << gui.moved
             << L", guiCursorUnmapped=" << gui.unmapped
             << L", guiCursorStale=" << gui.stale
             << L", guiCursorFailures=" << gui.failures
             << L", guiFollowersFound=" << gui.followersFound
             << L", guiFollowersMoved=" << gui.followersMoved
             << L", guiCursorMode=" << gui.mode
             << L", guiLargestMove=" << gui.largestMove
             << L", guiSlope=" << gui.slopeX << L"/" << gui.slopeY;
    }
    if (g_worldTraceEnabled) {
        const world::Counters worldCounters = world::TakeCounters();
        line << L", worldTracked=" << worldCounters.tracked
             << L", worldMoving=" << worldCounters.moving
             << L", worldEligible=" << worldCounters.eligible
             << L", worldApplied=" << worldCounters.applied
             << L", worldUnavailable=" << worldCounters.unavailable
             << L", worldRejected=" << worldCounters.rejected
             << L", worldInvalid=" << worldCounters.invalid
             << L", worldSameTickChanges=" << worldCounters.sameTickChanges
             << L", worldFrameVariants=" << worldCounters.frameVariants
             << L", worldQuarantined=" << worldCounters.quarantined
             << L", worldTableFull=" << worldCounters.tableFull
             << L", worldEvictions=" << worldCounters.evictions
             << L", worldUpdateCountRefreshes="
             << worldCounters.updateCountRefreshes
             << L", worldLed=" << worldCounters.led;
        line << L", lightProjectionApplied="
             << g_lightProjectionApplied.exchange(0,
                                                  std::memory_order_acq_rel)
             << L", lightProjectionRejected="
             << g_lightProjectionRejected.exchange(0,
                                                   std::memory_order_acq_rel)
             << L", lightProjectionOverflows="
             << g_lightProjectionOverflows.exchange(
                    0, std::memory_order_acq_rel)
             << L", lightShadowChecks="
             << g_lightShadowChecks.exchange(0, std::memory_order_acq_rel)
             << L", lightShadowForced="
             << g_lightShadowForced.exchange(0, std::memory_order_acq_rel)
             << L", lightViewApplied="
             << g_lightViewApplied.exchange(0, std::memory_order_acq_rel)
             << L", lightViewRejected="
             << g_lightViewRejected.exchange(0, std::memory_order_acq_rel)
             << L", lightFrustumRebuilds="
             << g_lightFrustumRebuilds.exchange(0,
                                                std::memory_order_acq_rel);
        const world::PoseCounters poseCounters = world::TakePoseCounters();
        line << L", worldPoseTracked=" << poseCounters.tracked
             << L", worldPoseChanged=" << poseCounters.changed
             << L", worldPoseApplied=" << poseCounters.applied
             << L", worldPoseUnavailable=" << poseCounters.unavailable
             << L", worldPoseRejected=" << poseCounters.rejected
             << L", worldPoseSnappedBones=" << poseCounters.snappedBones
             << L", worldPoseSameTickChanges="
             << poseCounters.sameTickChanges
             << L", worldPoseQuarantined=" << poseCounters.quarantined
             << L", worldPoseOversize=" << poseCounters.oversize
             << L", worldPoseTableFull=" << poseCounters.tableFull
             << L", worldPoseMaxBones=" << poseCounters.maximumBones
             << L", worldPoseChangedMaxBones="
             << poseCounters.changedMaximumBones;
    }
    log::Write(log::Level::info, line.str());
}

void RegisterTelemetryThread() {
    if (!g_telemetryEnabled) {
        return;
    }
    const DWORD current = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_telemetryThread.compare_exchange_strong(
            expected, current, std::memory_order_relaxed) &&
        expected != current) {
        g_threadMismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void SampleSchedulerPhase() {
    const std::uintptr_t timerAddress =
        g_logicTimerPointer.load(std::memory_order_relaxed);
    if (timerAddress == 0 || g_engineMicrosecondClock == nullptr) {
        g_invalidPhaseSamples.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto* const timer = reinterpret_cast<const std::uint8_t*>(timerAddress);
    std::int64_t timerTickMicroseconds = 0;
    double stepMicroseconds = 0.0;
    double speedMultiplier = 0.0;
    std::memcpy(&timerTickMicroseconds, timer, sizeof(timerTickMicroseconds));
    std::memcpy(&stepMicroseconds, timer + 0x08, sizeof(stepMicroseconds));
    std::memcpy(&speedMultiplier, timer + 0x30, sizeof(speedMultiplier));
    const std::int64_t nowMicroseconds = g_engineMicrosecondClock();
    const double timerTickMilliseconds =
        static_cast<double>(timerTickMicroseconds) / 1000.0;
    const double stepMilliseconds = stepMicroseconds / 1000.0;
    const double nowMilliseconds =
        static_cast<double>(nowMicroseconds) / 1000.0;

    double completedCandidateAlpha = 0.0;
    double completedCandidateRaw = 0.0;
    if (clock::LogicTimerAlphaFromCompletedTick(
            timerTickMilliseconds, stepMilliseconds, speedMultiplier,
            nowMilliseconds, 0.0, completedCandidateAlpha,
            &completedCandidateRaw)) {
        g_completedFieldValid.fetch_add(1, std::memory_order_relaxed);
    } else if (completedCandidateRaw < -0.25) {
        g_completedFieldUnderRange.fetch_add(1, std::memory_order_relaxed);
    } else if (completedCandidateRaw > 1.25) {
        g_completedFieldOverRange.fetch_add(1, std::memory_order_relaxed);
    }

    const clock::LogicPhase phase = clock::LogicTimerPresentationPhase(
        timerTickMilliseconds, stepMilliseconds, speedMultiplier,
        nowMilliseconds, 0.0);
    if (phase.kind == clock::LogicPhaseKind::unavailable) {
        if (phase.rawAlpha < -0.25) {
            g_nextFieldUnderRange.fetch_add(1, std::memory_order_relaxed);
        }
        g_invalidPhaseSamples.fetch_add(1, std::memory_order_relaxed);
        g_hasLastPhase = false;
        return;
    }
    const double alpha = phase.alpha;
    if (phase.kind == clock::LogicPhaseKind::holdCurrent) {
        g_phaseHolds.fetch_add(1, std::memory_order_relaxed);
        g_nextFieldOverRange.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t serial = g_fixedSerial.load(std::memory_order_relaxed);
    if (g_hasLastPhase && serial == g_lastPhaseSerial &&
        alpha + 0.001 < g_lastPhaseAlpha) {
        g_phaseRegressions.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastPhaseSerial = serial;
    g_lastPhaseAlpha = alpha;
    g_hasLastPhase = true;
    g_latestAlphaPartsPerMillion.store(
        static_cast<std::uint32_t>(alpha * 1'000'000.0 + 0.5),
        std::memory_order_relaxed);
    g_phaseSamples.fetch_add(1, std::memory_order_relaxed);
}

void CaptureCameraInterpolationEndpoint() {
    if (!g_cameraInterpolation || g_engineMicrosecondClock == nullptr) {
        return;
    }
    const std::uintptr_t timerAddress =
        g_logicTimerPointer.load(std::memory_order_relaxed);
    if (timerAddress == 0) {
        g_cameraInterpolationSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto* const timer = reinterpret_cast<const std::uint8_t*>(timerAddress);
    double stepMicroseconds = 0.0;
    double speedMultiplier = 0.0;
    std::memcpy(&stepMicroseconds, timer + 0x08, sizeof(stepMicroseconds));
    std::memcpy(&speedMultiplier, timer + 0x30, sizeof(speedMultiplier));
    const double stepSeconds =
        stepMicroseconds / (1'000'000.0 * speedMultiplier);
    if (!std::isfinite(stepSeconds) || stepSeconds < 0.001 ||
        stepSeconds > 1.0) {
        g_cameraInterpolationFailures.fetch_add(1, std::memory_order_relaxed);
        g_cameraRuntime.Reset();
        g_cameraClock.Reset(clock::ResetReason::invalidConfiguration);
        return;
    }
    if (!g_cameraClock.IsConfigured() ||
        std::abs(stepSeconds - g_cameraClockStepSeconds) > 1.0e-9) {
        if (!g_cameraClock.Configure(stepSeconds)) {
            g_cameraInterpolationFailures.fetch_add(1,
                                                    std::memory_order_relaxed);
            g_cameraRuntime.Reset();
            return;
        }
        g_cameraClockStepSeconds = stepSeconds;
        g_cameraRuntime.Reset();
        g_cameraInterpolationResets.fetch_add(1, std::memory_order_relaxed);
    }
    const std::int64_t nowMicroseconds = g_engineMicrosecondClock();
    if (nowMicroseconds < 0) {
        g_cameraInterpolationFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t serial =
        g_fixedSerial.load(std::memory_order_relaxed);
    const clock::PublishResult published = g_cameraClock.Publish(
        serial, static_cast<double>(nowMicroseconds) / 1'000'000.0);
    if (!published.accepted) {
        g_cameraInterpolationFailures.fetch_add(1, std::memory_order_relaxed);
        g_cameraRuntime.Reset();
        return;
    }
    if (published.resetReason != clock::ResetReason::none) {
        g_cameraInterpolationResets.fetch_add(1, std::memory_order_relaxed);
    }
    void* scene = nullptr;
    std::memcpy(&scene, reinterpret_cast<const void*>(g_sceneGlobalPointer),
                sizeof(scene));
    const camera::TransactionResult captured = g_cameraRuntime.Capture(
        scene, serial, g_cameraClock.Generation());
    if (captured == camera::TransactionResult::captured) {
        g_cameraInterpolationCaptures.fetch_add(1, std::memory_order_relaxed);
    } else if (captured == camera::TransactionResult::identityChanged ||
               captured == camera::TransactionResult::discontinuityReset) {
        g_cameraInterpolationResets.fetch_add(1, std::memory_order_relaxed);
    } else if (captured == camera::TransactionResult::noCurrentListener ||
               captured == camera::TransactionResult::listenerHidden ||
               captured == camera::TransactionResult::listenerMissingCamera ||
               captured == camera::TransactionResult::listenerMissingWorld ||
               captured == camera::TransactionResult::unsupportedRotateMode) {
        g_cameraInterpolationSkipped.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_cameraInterpolationFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

clock::Sample EvaluateCameraInterpolationClock() {
    if (!g_cameraInterpolation || !g_cameraClock.IsConfigured() ||
        g_engineMicrosecondClock == nullptr) {
        return {};
    }
    const std::uintptr_t timerAddress =
        g_logicTimerPointer.load(std::memory_order_relaxed);
    if (timerAddress == 0) {
        return {};
    }
    const auto* const timer = reinterpret_cast<const std::uint8_t*>(timerAddress);
    std::int64_t nextTickMicroseconds = 0;
    double stepMicroseconds = 0.0;
    double speedMultiplier = 0.0;
    std::memcpy(&nextTickMicroseconds, timer, sizeof(nextTickMicroseconds));
    std::memcpy(&stepMicroseconds, timer + 0x08, sizeof(stepMicroseconds));
    std::memcpy(&speedMultiplier, timer + 0x30, sizeof(speedMultiplier));
    const std::int64_t nowMicroseconds = g_engineMicrosecondClock();
    if (nowMicroseconds < 0) {
        return {};
    }
    const clock::LogicPhase phase = clock::LogicTimerPresentationPhase(
        static_cast<double>(nextTickMicroseconds) / 1000.0,
        stepMicroseconds / 1000.0, speedMultiplier,
        static_cast<double>(nowMicroseconds) / 1000.0, 0.0);
    return g_cameraClock.Evaluate(
        static_cast<double>(nowMicroseconds) / 1'000'000.0, phase.alpha,
        phase.kind != clock::LogicPhaseKind::unavailable);
}

void WaitUntil(const std::int64_t deadline) {
    for (;;) {
        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter) || counter.QuadPart >= deadline) {
            return;
        }
        const std::int64_t remaining = deadline - counter.QuadPart;
        if (remaining > g_spinThresholdTicks && g_waitableTimer != nullptr) {
            const std::int64_t coarseTicks = remaining - g_spinThresholdTicks;
            const auto coarseHundredNanoseconds = static_cast<LONGLONG>(
                (static_cast<long double>(coarseTicks) * 10'000'000.0L) /
                static_cast<long double>(g_qpcFrequency));
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(coarseHundredNanoseconds, 1);
            if (SetWaitableTimer(g_waitableTimer, &due, 0, nullptr, nullptr,
                                 FALSE) &&
                WaitForSingleObject(g_waitableTimer, INFINITE) == WAIT_OBJECT_0) {
                continue;
            }
            if (!g_waitFailureLogged.exchange(true, std::memory_order_acq_rel)) {
                log::Write(log::Level::warning,
                           L"Presentation limiter waitable timer failed; using "
                           L"the QPC fallback for this process.");
            }
        }
        YieldProcessor();
    }
}

void ApplyRenderUnlock() {
    if (!g_renderUnlock || g_engineGlobalPointer == 0) {
        return;
    }
    void* engine = nullptr;
    std::memcpy(&engine, reinterpret_cast<const void*>(g_engineGlobalPointer),
                sizeof(engine));
    if (engine == nullptr) {
        return;
    }
    auto* const limitFps = static_cast<std::uint8_t*>(engine) + 0x88;
    constexpr std::uint8_t disabled = 0;
    std::memcpy(limitFps, &disabled, sizeof(disabled));
    if (!g_renderUnlockLogged.exchange(true, std::memory_order_acq_rel)) {
        log::Write(log::Level::info,
                   L"RenderUnlock applied: cEngine::LimitFPS=false at field +0x88.");
    }
}

// Opt-in: holds the authoritative update rate at the configured stock value
// by calling the engine's own setter, the same call the Framerate menu makes.
// The per-pass catch-up limit the game derived for its own rate is kept.
void ApplyGameplayRateOverride() {
    if (g_gameplayRateOverride == 0 || g_engineSetUpdatesPerSec == nullptr ||
        g_engineGlobalPointer == 0) {
        return;
    }
    void* engine = nullptr;
    std::memcpy(&engine, reinterpret_cast<const void*>(g_engineGlobalPointer),
                sizeof(engine));
    if (engine == nullptr) {
        return;
    }
    const std::uint8_t* timer = nullptr;
    std::memcpy(&timer, static_cast<const std::uint8_t*>(engine) + 0x48,
                sizeof(timer));
    if (timer == nullptr) {
        return;
    }
    int currentRate = 0;
    int updatesPerPass = 0;
    std::memcpy(&currentRate, timer + 0x28, sizeof(currentRate));
    std::memcpy(&updatesPerPass, timer + 0x38, sizeof(updatesPerPass));
    if (currentRate == g_gameplayRateOverride || currentRate <= 0 ||
        updatesPerPass <= 0) {
        return;
    }
    const int catchUpDivisor =
        std::max(1, g_gameplayRateOverride / updatesPerPass);
    g_engineSetUpdatesPerSec(engine, g_gameplayRateOverride, catchUpDivisor);
    if (g_gameplayRateApplications.fetch_add(
            1, std::memory_order_relaxed) == 0) {
        log::Write(log::Level::info,
                   L"GameplayRateOverride applied: engine update rate " +
                       std::to_wstring(currentRate) + L" -> " +
                       std::to_wstring(g_gameplayRateOverride) +
                       L" through cEngine::SetUpdatesPerSec.");
    }
}

// When the simulation's next tick falls due and how long a tick is, in
// performance-counter ticks.
bool ReadTickSchedule(const std::int64_t now,
                      std::int64_t& due,
                      std::int64_t& period) {
    const std::uintptr_t timerAddress =
        g_logicTimerPointer.load(std::memory_order_relaxed);
    if (timerAddress == 0 || g_engineMicrosecondClock == nullptr ||
        g_qpcFrequency <= 0) {
        return false;
    }
    const auto* const timer = reinterpret_cast<const std::uint8_t*>(timerAddress);
    std::int64_t nextTickMicroseconds = 0;
    double stepMicroseconds = 0.0;
    double speedMultiplier = 0.0;
    std::memcpy(&nextTickMicroseconds, timer, sizeof(nextTickMicroseconds));
    std::memcpy(&stepMicroseconds, timer + 0x08, sizeof(stepMicroseconds));
    std::memcpy(&speedMultiplier, timer + 0x30, sizeof(speedMultiplier));
    if (!std::isfinite(stepMicroseconds) || !std::isfinite(speedMultiplier) ||
        stepMicroseconds < 1000.0 || stepMicroseconds > 1'000'000.0 ||
        speedMultiplier < 0.01 || speedMultiplier > 100.0) {
        return false;
    }
    const std::int64_t engineNow = g_engineMicrosecondClock();
    const double perMicrosecond = static_cast<double>(g_qpcFrequency) / 1.0e6;
    const double untilDue =
        static_cast<double>(nextTickMicroseconds - engineNow) * perMicrosecond;
    if (engineNow < 0 || !std::isfinite(untilDue) ||
        std::abs(untilDue) > static_cast<double>(g_qpcFrequency) * 10.0) {
        return false;
    }
    due = now + static_cast<std::int64_t>(untilDue);
    period = static_cast<std::int64_t>(stepMicroseconds / speedMultiplier *
                                       perMicrosecond);
    return true;
}

// Plans this frame's present once, at the first point of the frame that
// reaches the plugin. Before a render it also holds the render back, so the
// view is sampled the same time before every present.
void BeginPacedFrame(const bool beforeRender) {
    if (!g_evenFramePacing || g_pacePlanned) {
        return;
    }
    g_pacePlanned = true;
    g_pacePlan = {};
    LARGE_INTEGER now{};
    std::int64_t due = 0;
    std::int64_t period = 0;
    if (!QueryPerformanceCounter(&now) ||
        !ReadTickSchedule(now.QuadPart, due, period)) {
        return;
    }
    g_pacePlan = g_evenPacer.BeginFrame(now.QuadPart, due, period);
    if (!g_pacePlan.valid || !beforeRender) {
        return;
    }
    g_paceRenderStart = now.QuadPart;
    if (now.QuadPart < g_pacePlan.renderStart) {
        WaitUntil(g_pacePlan.renderStart);
        LARGE_INTEGER after{};
        if (QueryPerformanceCounter(&after)) {
            g_paceWaited = after.QuadPart - now.QuadPart;
            g_paceRenderStart = after.QuadPart;
        }
    }
}

// Returns true if the even pacer handled this frame's present.
bool PresentPacedFrame(const std::int64_t now) {
    BeginPacedFrame(false);
    if (!g_pacePlan.valid) {
        if (g_evenFramePacing) {
            g_paceFallbackFrames.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    g_pacedFrames.fetch_add(1, std::memory_order_relaxed);
    if (g_paceFrameStart != 0) {
        g_evenPacer.EndFrame(
            now - g_paceFrameStart - g_paceWaited,
            g_paceRenderStart != 0 ? now - g_paceRenderStart : 0,
            g_paceFrameTicks);
    }
    if (g_paceTickWasDue && g_paceFrameTicks == 0) {
        g_paceTickMisses.fetch_add(1, std::memory_order_relaxed);
    }
    if (now < g_pacePlan.deadline) {
        g_limiterWaits.fetch_add(1, std::memory_order_relaxed);
        WaitUntil(g_pacePlan.deadline);
    } else if (now - g_pacePlan.deadline > g_qpcFrequency / 10000) {
        g_paceLateFrames.fetch_add(1, std::memory_order_relaxed);
    }
    frame_trace::SetFramesPerTick(g_evenPacer.FramesPerTick());
    return true;
}

void HookedSteadySwap(void* const graphics) {
    RegisterTelemetryThread();
    frame_trace::Mark(frame_trace::Stamp::swapEnter);
    ApplyRenderUnlock();
    ApplyGameplayRateOverride();
    LARGE_INTEGER before{};
    if (g_presentationLimiter && QueryPerformanceCounter(&before) &&
        !PresentPacedFrame(before.QuadPart)) {
        {
            const limiter::WaitPlan plan =
                g_deadlineSchedule.BeforePresent(before.QuadPart);
            if (plan.phaseReset) {
                g_limiterPhaseResets.fetch_add(1, std::memory_order_relaxed);
            }
            if (plan.shouldWait) {
                g_limiterWaits.fetch_add(1, std::memory_order_relaxed);
                WaitUntil(plan.deadline);
            }
        }
    }
    frame_trace::Mark(frame_trace::Stamp::waitEnd);
    if (g_evenFramePacing) {
        LARGE_INTEGER frameStart{};
        std::int64_t due = 0;
        std::int64_t period = 0;
        if (QueryPerformanceCounter(&frameStart)) {
            g_paceFrameStart = frameStart.QuadPart;
            g_paceTickWasDue =
                ReadTickSchedule(frameStart.QuadPart, due, period) &&
                due <= frameStart.QuadPart;
        }
        g_pacePlanned = false;
        g_paceWaited = 0;
        g_paceRenderStart = 0;
        g_paceFrameTicks = 0;
    }
    g_originalSwap(graphics);
    SamplePresentedCamera();

    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || g_qpcFrequency <= 0) {
        return;
    }
    frame_trace::EndFrame(counter.QuadPart);
    if (g_presentationLimiter) {
        g_deadlineSchedule.Presented(counter.QuadPart);
    }
    if (!g_telemetryEnabled) {
        return;
    }
    SampleSchedulerPhase();
    if (g_mouseTraceEnabled) {
        mouse::OnPresent();
    }
    g_presentCadence.RecordQpc(counter.QuadPart, g_qpcFrequency);

    std::int64_t previous = g_lastReportCounter.load(std::memory_order_relaxed);
    if (previous == 0) {
        (void)g_lastReportCounter.compare_exchange_strong(
            previous, counter.QuadPart, std::memory_order_relaxed);
        return;
    }
    if (counter.QuadPart - previous < g_reportIntervalTicks ||
        !g_lastReportCounter.compare_exchange_strong(
            previous, counter.QuadPart, std::memory_order_acq_rel)) {
        return;
    }
    // The counters line is long and only of use when reporting a problem.
    if (log::DiagnosticsEnabled()) {
        ReportCadence(counter.QuadPart - previous);
    }
}

std::uint8_t HookedLogicTimerWantUpdate(void* const timer,
                                        const int maximumUpdates) {
    RegisterTelemetryThread();
    g_logicTimerPointer.store(reinterpret_cast<std::uintptr_t>(timer),
                              std::memory_order_relaxed);
    const std::uint8_t result = g_originalWantUpdate(timer, maximumUpdates);
    if (result != 0) {
        g_fixedUpdates.fetch_add(1, std::memory_order_relaxed);
        frame_trace::Mark(frame_trace::Stamp::fixedBegin);
        frame_trace::CountFixedUpdate();
        ++g_paceFrameTicks;
    }
    if (result != 0 && timer != nullptr) {
        std::uint32_t configuredRate = 0;
        std::uint32_t updatesThisFrame = 0;
        std::memcpy(&configuredRate,
                    static_cast<const std::uint8_t*>(timer) + 0x28,
                    sizeof(configuredRate));
        std::memcpy(&updatesThisFrame,
                    static_cast<const std::uint8_t*>(timer) + 0x3C,
                    sizeof(updatesThisFrame));
        g_configuredUpdateRate.store(configuredRate, std::memory_order_relaxed);
        g_latestUpdatesThisFrame.store(updatesThisFrame,
                                       std::memory_order_relaxed);
    }
    return result;
}

void HookedUpdaterDispatch(void* const updater,
                           const int message,
                           const float stepSeconds) {
    RegisterTelemetryThread();
    g_originalUpdaterDispatch(updater, message, stepSeconds);
    switch (message) {
    case 5: // _PostUpdate: all fixed-update listeners have returned.
        g_completedFixedUpdates.fetch_add(1, std::memory_order_relaxed);
        g_fixedSerial.fetch_add(1, std::memory_order_relaxed);
        SampleCompletedCamera();
        frame_trace::Mark(frame_trace::Stamp::fixedEnd);
        break;
    case 7: // _PostVariableUpdate
        g_variableUpdates.fetch_add(1, std::memory_order_relaxed);
        SampleVariableCamera();
        CaptureCameraInterpolationEndpoint();
        frame_trace::Mark(frame_trace::Stamp::variableEnd);
        break;
    case 2: // _OnDraw
        g_drawFrames.fetch_add(1, std::memory_order_relaxed);
        SampleDrawCamera();
        break;
    case 0: // end-of-frame updater message after swap/finalization
        g_endFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void HookedEntitySetMatrix(void* const entity,
                           const hands_trace::Matrix* const matrix) {
    RegisterTelemetryThread();
    g_originalEntitySetMatrix(entity, matrix);
    if (!g_handsTraceEnabled) {
        return;
    }
    g_entitySetMatrixCalls.fetch_add(1, std::memory_order_relaxed);
    if (entity == nullptr || matrix == nullptr) {
        g_handsTraceInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!hands_trace::IsFiniteAffine(*matrix)) {
        g_handsTraceInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_entitySetMatrixFinite.fetch_add(1, std::memory_order_relaxed);
    camera_trace::Sample camera{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, camera) !=
        camera_trace::SampleStatus::ok) {
        return;
    }
    g_entitySetMatrixCameraReady.fetch_add(1, std::memory_order_relaxed);
    const float distance = hands_trace::DistanceFrom(*matrix, camera.position);
    const auto basisLengths = hands_trace::BasisLengths(*matrix);
    const auto toMillis = [](const float value) {
        return static_cast<std::uint32_t>(std::clamp(
            std::lround(static_cast<double>(value) * 1000.0), 0L, 100000L));
    };
    g_latestSetMatrixScaleXMillis.store(toMillis(basisLengths[0]),
                                        std::memory_order_relaxed);
    g_latestSetMatrixScaleYMillis.store(toMillis(basisLengths[1]),
                                        std::memory_order_relaxed);
    g_latestSetMatrixScaleZMillis.store(toMillis(basisLengths[2]),
                                        std::memory_order_relaxed);
    g_latestSetMatrixDistanceMillis.store(toMillis(distance),
                                          std::memory_order_relaxed);
    g_latestSetMatrixEntity.store(reinterpret_cast<std::uintptr_t>(entity),
                                  std::memory_order_relaxed);
    if (distance <= 2.0F) {
        g_entitySetMatrixNearCamera.fetch_add(1, std::memory_order_relaxed);
        RecordSetMatrixMeshEntity(entity, hands_trace::GetMeshEntity(entity),
                                  *matrix, distance);
    }
    if (distance <= 0.5F) {
        g_entitySetMatrixWithinHalfMeter.fetch_add(1,
                                                  std::memory_order_relaxed);
    }
    if (hands_trace::HasUniformScale(*matrix, 0.25F)) {
        g_entitySetMatrixQuarterScale.fetch_add(1, std::memory_order_relaxed);
    } else if (hands_trace::HasUniformScale(*matrix, 1.0F)) {
        g_entitySetMatrixUnitScale.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_entitySetMatrixOtherScale.fetch_add(1, std::memory_order_relaxed);
    }
    if (!hands_trace::IsOrdinaryHandsRoot(*matrix, camera.position)) {
        return;
    }
    g_handsRootWrites.fetch_add(1, std::memory_order_relaxed);
    RecordTrackedEntity(g_handsRootState, entity, *matrix,
                        g_handsRootChanges, g_handsRootStable,
                        g_handsRootIdentityChanges);
    g_handsRootMeshEntity =
        reinterpret_cast<std::uintptr_t>(hands_trace::GetMeshEntity(entity));
}

void HookedEntityUpdateAttachment(void* const entity) {
    RegisterTelemetryThread();
    g_originalEntityUpdateAttachment(entity);
    if (!g_handsTraceEnabled || entity == nullptr) {
        return;
    }
    g_attachmentCalls.fetch_add(1, std::memory_order_relaxed);
    camera_trace::Sample camera{};
    hands_trace::Matrix matrix{};
    if (camera_trace::ReadCurrentListenerCamera(g_sceneGlobalPointer, camera) !=
        camera_trace::SampleStatus::ok) {
        return;
    }
    if (!hands_trace::ReadEntityMatrix(entity, matrix)) {
        g_handsTraceInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!hands_trace::IsNearCamera(matrix, camera.position)) {
        return;
    }
    g_attachmentNearCamera.fetch_add(1, std::memory_order_relaxed);
    g_latestAttachmentPointer.store(reinterpret_cast<std::uintptr_t>(entity),
                                    std::memory_order_relaxed);
    RecordAttachmentEntity(entity, hands_trace::GetMeshEntity(entity), matrix,
                           hands_trace::DistanceFrom(matrix,
                                                     camera.position));
}

// Handles the first-person owners. `matched` reports whether the submesh
// belongs to one, in which case the world layer must leave it alone.
hands_trace::Matrix* PresentFirstPersonModelMatrix(
    void* const subMesh,
    hands_trace::Matrix* const original,
    bool& matched) noexcept {
    matched = false;
    if (!g_handsTraceEnabled || g_renderOwnerCount == 0) {
        return original;
    }

    // cSubMeshEntity is live for the duration of this render callback. Only
    // read its cMeshEntity owner field; retained identities are compared as
    // opaque pointer values and are never dereferenced here.
    constexpr std::size_t kSubMeshOwnerOffset = 0x1B0;
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) +
                    kSubMeshOwnerOffset,
                sizeof(owner));
    if (owner == nullptr) {
        return original;
    }
    const std::uintptr_t ownerValue =
        reinterpret_cast<std::uintptr_t>(owner);
    for (std::size_t index = 0; index < g_renderOwnerCount; ++index) {
        const RenderOwnerEntry& candidate = g_renderOwners[index];
        if (ownerValue != candidate.owner) {
            continue;
        }
        if (candidate.kind != RenderOwnerKind::setMatrix) {
            // An entity attached to another one (an item inside a carried
            // container, a prop on the player) is traced here but presented
            // by the world layer like any other moving object.
            g_subMeshModelAttachmentMatches.fetch_add(
                1, std::memory_order_relaxed);
            return original;
        }
        matched = true;
        const RenderStateLock lock;
        if (candidate.kind == RenderOwnerKind::setMatrix) {
            g_subMeshModelSetMatrixMatches.fetch_add(
                1, std::memory_order_relaxed);
            if (candidate.state != nullptr) {
                ++candidate.state->intervalRenderMatches;
                if (hands_trace::SameMatrix(*original,
                                            candidate.state->latest)) {
                    ++candidate.state->intervalExactMatrixMatches;
                } else {
                    ++candidate.state->intervalDifferentMatrixMatches;
                }
                RecordRelativeModelMatrices(*candidate.state, *original);
            }
            g_latestSetMatrixRenderOwner.store(candidate.owner,
                                               std::memory_order_relaxed);
            g_latestSetMatrixRenderEntity.store(candidate.entity,
                                                std::memory_order_relaxed);
            g_latestSetMatrixRenderDistanceMillis.store(
                candidate.distanceMillis, std::memory_order_relaxed);
            if (g_handsRootInterpolation &&
                candidate.distanceMillis <=
                    kFirstPersonOwnerMaxDistanceMillis &&
                candidate.state != nullptr) {
                hands_trace::Matrix presented{};
                HandsInterpolationResult interpolation =
                    InterpolateHandsModelMatrix(*candidate.state, *original,
                                                presented);
                if (interpolation == HandsInterpolationResult::applied) {
                    hands_trace::Matrix* const slot =
                        AcquirePresentedModelMatrix(subMesh);
                    if (slot != nullptr) {
                        if (g_viewLeadActive) {
                            // The body and held item ride with the view.
                            world::ApplyLead(g_viewLead, presented);
                        }
                        *slot = presented;
                        g_handsInterpolationApplied.fetch_add(
                            1, std::memory_order_relaxed);
                        if (g_worldRootInterpolation &&
                            !hands_trace::SameMatrix(
                                candidate.state->previous,
                                candidate.state->latest)) {
                            // Keeps shadow maps that contain the player's
                            // own body and held item in step with it, but
                            // only while it is actually moving: every
                            // refresh makes those lights redraw their maps.
                            world::MarkPresented(subMesh);
                        }
                        return slot;
                    }
                    interpolation = HandsInterpolationResult::unavailable;
                }
                if (interpolation == HandsInterpolationResult::unavailable) {
                    g_handsInterpolationSkipped.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    g_handsInterpolationFailures.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            if (g_handsRenderCopyProbe && candidate.distanceMillis <= 1000U) {
                std::memcpy(&g_handsRenderMatrixCopy, original,
                            sizeof(g_handsRenderMatrixCopy));
                g_handsRenderCopyReturns.fetch_add(
                    1, std::memory_order_relaxed);
                return &g_handsRenderMatrixCopy;
            }
        } else {
            g_subMeshModelAttachmentMatches.fetch_add(
                1, std::memory_order_relaxed);
        }
        return original;
    }
    return original;
}

hands_trace::Matrix* HookedSubMeshGetModelMatrix(void* const subMesh,
                                                  void* const frustum) {
    hands_trace::Matrix* const original =
        g_originalSubMeshGetModelMatrix(subMesh, frustum);
    if (original == nullptr || subMesh == nullptr) {
        return original;
    }
    bool firstPerson = false;
    hands_trace::Matrix* const presented =
        PresentFirstPersonModelMatrix(subMesh, original, firstPerson);
    if (firstPerson || !g_worldTraceEnabled) {
        return presented;
    }
    // The engine's signature is non-const, but the renderer only reads the
    // returned matrix; the proven first-person path relies on the same fact.
    return const_cast<hands_trace::Matrix*>(
        world::Present(subMesh, original));
}

struct PresentedLightProjection {
    void* light = nullptr;
    std::uint64_t renderSerial = 0;
    alignas(16) hands_trace::Matrix matrix{};
};

std::array<PresentedLightProjection, 16> g_presentedLightProjections{};

// A spot light's shadow and gobo projection is derived from its world matrix,
// which only changes on a fixed tick. When the world layer is presenting the
// light at an interpolated position, rebuild the same product from that
// position so the shadows it casts move with the lit cone. The engine's own
// cache is left untouched, and the formula is checked against the engine's
// result for the authoritative matrix on every call before it is trusted.
hands_trace::Matrix* HookedLightSpotGetViewProjMatrix(void* const light) {
    hands_trace::Matrix* const original =
        g_originalLightSpotGetViewProjMatrix(light);
    if (original == nullptr || light == nullptr ||
        g_originalSubMeshGetModelMatrix == nullptr) {
        return original;
    }
    hands_trace::Matrix* const live =
        g_originalSubMeshGetModelMatrix(light, nullptr);
    if (live == nullptr) {
        return original;
    }
    const hands_trace::Matrix* const presented = world::Present(light, live);
    if (presented == live) {
        return original;
    }
    const hands_trace::Matrix* const projection =
        g_lightSpotGetProjectionMatrix(light);
    hands_trace::Matrix liveView{};
    hands_trace::Matrix presentedView{};
    if (projection == nullptr ||
        !hands_trace::InvertAffine(*live, liveView) ||
        !hands_trace::InvertAffine(*presented, presentedView)) {
        g_lightProjectionRejected.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    const hands_trace::Matrix reference = hands_trace::Multiply(
        *g_lightProjectionBias, hands_trace::Multiply(*projection, liveView));
    float magnitude = 1.0F;
    for (const float value : original->values) {
        magnitude = std::max(magnitude, std::abs(value));
    }
    const float difference =
        hands_trace::MaximumAbsoluteDifference(reference, *original);
    if (!(difference <= 1.0e-3F * magnitude)) {
        g_lightProjectionRejected.fetch_add(1, std::memory_order_relaxed);
        return original;
    }

    const RenderStateLock lock;
    PresentedLightProjection* slot = nullptr;
    for (auto& entry : g_presentedLightProjections) {
        if (entry.renderSerial == g_handsPoseRenderSerial) {
            if (entry.light == light) {
                return &entry.matrix;
            }
        } else if (slot == nullptr) {
            slot = &entry;
        }
    }
    if (slot == nullptr) {
        g_lightProjectionOverflows.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    slot->light = light;
    slot->renderSerial = g_handsPoseRenderSerial;
    slot->matrix = hands_trace::Multiply(
        *g_lightProjectionBias,
        hands_trace::Multiply(*projection, presentedView));
    g_lightProjectionApplied.fetch_add(1, std::memory_order_relaxed);
    return &slot->matrix;
}

std::array<PresentedLightProjection, 16> g_presentedLightViews{};
std::array<void*, 16> g_lightFrustumsToRestore{};
std::size_t g_lightFrustumsToRestoreCount = 0;

// Supplies inverse(interpolatedWorld) to the frustum rebuild below. The
// engine's own cached view matrix is computed first and left untouched.
hands_trace::Matrix* HookedLightSpotGetViewMatrix(void* const light) {
    hands_trace::Matrix* const original =
        g_originalLightSpotGetViewMatrix(light);
    if (!t_lightInterpolatedViewScope || original == nullptr ||
        light == nullptr || g_originalSubMeshGetModelMatrix == nullptr) {
        return original;
    }
    hands_trace::Matrix* const live =
        g_originalSubMeshGetModelMatrix(light, nullptr);
    if (live == nullptr) {
        return original;
    }
    const hands_trace::Matrix* const presented = world::Present(light, live);
    if (presented == live) {
        return original;
    }
    hands_trace::Matrix liveView{};
    hands_trace::Matrix presentedView{};
    if (!hands_trace::InvertAffine(*live, liveView) ||
        !hands_trace::InvertAffine(*presented, presentedView)) {
        g_lightViewRejected.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    float magnitude = 1.0F;
    for (const float value : original->values) {
        magnitude = std::max(magnitude, std::abs(value));
    }
    if (!(hands_trace::MaximumAbsoluteDifference(liveView, *original) <=
          1.0e-3F * magnitude)) {
        g_lightViewRejected.fetch_add(1, std::memory_order_relaxed);
        return original;
    }

    const RenderStateLock lock;
    PresentedLightProjection* slot = nullptr;
    for (auto& entry : g_presentedLightViews) {
        if (entry.renderSerial == g_handsPoseRenderSerial) {
            if (entry.light == light) {
                return &entry.matrix;
            }
        } else if (slot == nullptr) {
            slot = &entry;
        }
    }
    if (slot == nullptr) {
        g_lightProjectionOverflows.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    slot->light = light;
    slot->renderSerial = g_handsPoseRenderSerial;
    slot->matrix = presentedView;
    g_lightViewApplied.fetch_add(1, std::memory_order_relaxed);
    return &slot->matrix;
}

// The shadow map is drawn through the light's frustum, which the engine only
// rebuilds when the transform count changes. For a light presented at an
// interpolated position, make the engine rebuild it from the interpolated
// view on every render call, then leave the tag mismatched so the first call
// outside a render rebuilds it from authoritative state again.
void* HookedLightSpotGetFrustum(void* const light) {
    if (light == nullptr || !world::WasPresentedRecently(light)) {
        return g_originalLightSpotGetFrustum(light);
    }
    constexpr std::size_t kEntityTransformCountOffset = 0x78 + 0xE0;
    constexpr std::size_t kFrustumCountTagOffset = 0x4FC;
    auto* const bytes = static_cast<std::uint8_t*>(light);
    std::uint32_t count = 0;
    std::memcpy(&count, bytes + kEntityTransformCountOffset, sizeof(count));
    const std::uint32_t mismatched = count + 0x40000000U;
    std::memcpy(bytes + kFrustumCountTagOffset, &mismatched,
                sizeof(mismatched));
    t_lightInterpolatedViewScope = true;
    void* const frustum = g_originalLightSpotGetFrustum(light);
    t_lightInterpolatedViewScope = false;
    std::memcpy(bytes + kFrustumCountTagOffset, &mismatched,
                sizeof(mismatched));
    g_lightFrustumRebuilds.fetch_add(1, std::memory_order_relaxed);
    {
        const RenderStateLock lock;
        bool known = false;
        for (std::size_t index = 0; index < g_lightFrustumsToRestoreCount;
             ++index) {
            known = known || g_lightFrustumsToRestore[index] == light;
        }
        if (!known &&
            g_lightFrustumsToRestoreCount < g_lightFrustumsToRestore.size()) {
            g_lightFrustumsToRestore[g_lightFrustumsToRestoreCount++] = light;
        }
    }
    return frustum;
}

// Called right after the scene render returns, while every light touched in
// it is still alive: with the world layer inactive the hook above passes
// straight through, and the mismatched tag makes the engine rebuild each
// frustum from authoritative state before any gameplay code can read it.
void RestoreLightFrustums() noexcept {
    for (std::size_t index = 0; index < g_lightFrustumsToRestoreCount;
         ++index) {
        (void)g_originalLightSpotGetFrustum(g_lightFrustumsToRestore[index]);
    }
    g_lightFrustumsToRestoreCount = 0;
}

// The engine keeps a light's shadow map until the light or one of its
// casters reports a new transform, which only happens on a fixed tick. A light
// presented at an interpolated position gets a new projection every render,
// so its map has to be redrawn every render as well or the two disagree.
std::uint8_t HookedShadowMapNeedsUpdate(void* const renderer,
                                        void* const light,
                                        void* const shadowData,
                                        void* const casters,
                                        const std::uint8_t cacheUsable,
                                        const std::uint8_t flag) {
    const std::uint8_t needed = g_originalShadowMapNeedsUpdate(
        renderer, light, shadowData, casters, cacheUsable, flag);
    g_lightShadowChecks.fetch_add(1, std::memory_order_relaxed);
    if (needed != 0 || !world::WasPresentedRecently(light)) {
        return needed;
    }
    g_lightShadowForced.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int HookedRenderableGetMatrixUpdateCount(void* const renderable) {
    return world::RefreshUpdateCount(
        renderable, g_originalRenderableGetMatrixUpdateCount(renderable));
}

const void* HookedSubMeshGetSkinningPalette(void* const subMesh) {
    const void* const original = g_originalSubMeshGetSkinningPalette(subMesh);
    if (!g_handsPoseRenderActive || subMesh == nullptr ||
        original == nullptr) {
        return original;
    }
    g_handsPoseRendererCalls.fetch_add(1, std::memory_order_relaxed);
    void* owner = nullptr;
    std::memcpy(&owner, static_cast<const std::uint8_t*>(subMesh) + 0x1B0,
                sizeof(owner));
    if (owner == nullptr) {
        return original;
    }
    if (FindKnownRenderOwner(reinterpret_cast<std::uintptr_t>(owner)) ==
        nullptr) {
        if (g_worldTraceEnabled) {
            // Same live vector the engine getter just read from.
            std::uintptr_t begin = 0;
            std::uintptr_t end = 0;
            std::memcpy(&begin,
                        static_cast<const std::uint8_t*>(owner) + 0x318,
                        sizeof(begin));
            std::memcpy(&end, static_cast<const std::uint8_t*>(owner) + 0x320,
                        sizeof(end));
            if (begin == reinterpret_cast<std::uintptr_t>(original) &&
                end > begin &&
                (end - begin) % sizeof(SkinningDualQuaternion) == 0) {
                const void* const presented = world::PresentPalette(
                    owner, original,
                    (end - begin) / sizeof(SkinningDualQuaternion));
                if (presented != original) {
                    // The submesh's shadow has to follow its animation too.
                    world::MarkPresented(subMesh);
                }
                return presented;
            }
        }
        return original;
    }
    // Worker threads reach this point too, so the first caller of a frame
    // builds the interpolated pose under the lock and every caller copies it
    // into its own thread-local storage before the lock is released.
    const RenderStateLock lock;
    if (g_handsTraceEnabled) {
        RecordRendererPalette(subMesh, original);
    }
    if (g_handsPoseInterpolation) {
        return InterpolateFirstPersonRendererPalette(subMesh, original);
    }
    return CopyFirstPersonRendererPalette(subMesh, original);
}

void HookedSubMeshUpdateSkinning(void* const subMesh) {
    ArmHandsPoseProbe(subMesh);
    g_originalSubMeshUpdateSkinning(subMesh);
    FinishHandsPoseProbe();
    if (g_handsTraceEnabled) {
        RegisterTelemetryThread();
        RecordHandsPosePalette(subMesh);
    }
}

void HookedMeshUpdatePalette(void* const mesh) {
    g_originalMeshUpdatePalette(mesh);
    StageHandsPoseProbe(mesh);
}

void HookedSceneRenderAll(void* const scene,
                          const float frameTime,
                          const std::uint32_t renderFlags,
                          const float renderTime,
                          const std::uint32_t viewportMask) {
    RegisterTelemetryThread();
    frame_trace::Mark(frame_trace::Stamp::renderBegin);
    BeginPacedFrame(true);
    frame_trace::Mark(frame_trace::Stamp::paceWaitEnd);
    if (g_handsTraceEnabled) {
        SampleTrackedEntityAtRender(
            g_handsRootState, g_handsRootRenderSamples,
            g_handsRootRenderChanges, g_handsRootRenderRepeats);
        SampleAttachmentsAtRender();
        PrepareRenderOwners();
        ++g_handsPoseRenderSerial;
    }
    g_hasActiveHandsClockSample = false;
    camera::TransactionResult staged{};
    if (g_cameraInterpolation) {
        const clock::Sample sample = EvaluateCameraInterpolationClock();
        if (g_handsRootInterpolation &&
            sample.kind == clock::SampleKind::interpolated) {
            g_activeHandsClockSample = sample;
            g_hasActiveHandsClockSample = true;
        }
        camera::LookLead lookLead{};
        bool hasLookLead = false;
        if (g_mousePreview && sample.kind == clock::SampleKind::interpolated) {
            const mouse::Lead lead =
                mouse::ComputeLead(sample.newerSerial, sample.alpha);
            if (lead.active) {
                lookLead = {lead.yaw, lead.pitch, lead.pitchLow,
                            lead.pitchHigh};
                hasLookLead = true;
            }
        }
        camera::StagedLook look{};
        staged = g_cameraRuntime.BeginInterpolation(
            scene, sample, hasLookLead ? &lookLead : nullptr, &look);
        if (look.staged) {
            frame_trace::SetView(look.viewYaw, look.viewPitch, look.yaw,
                                 look.pitch, look.applied,
                                 static_cast<float>(sample.alpha),
                                 sample.newerSerial);
        }
        if (staged == camera::TransactionResult::interpolated &&
            look.applied &&
            mouse::BuildLeadRotation(look.yaw, look.pitch,
                                     g_viewLead.rotation)) {
            g_viewLead.center[0] = look.position.x;
            g_viewLead.center[1] = look.position.y;
            g_viewLead.center[2] = look.position.z;
            g_viewLead.radius = 1.0F;
            g_viewLeadActive = true;
            g_viewLeadFrames.fetch_add(1, std::memory_order_relaxed);
        }
        if (static_cast<std::size_t>(staged) < kCameraStageResultCount) {
            g_cameraStageResults[static_cast<std::size_t>(staged)].fetch_add(
                1, std::memory_order_relaxed);
        }
        if (staged == camera::TransactionResult::interpolated) {
            g_cameraInterpolationApplied.fetch_add(1,
                                                   std::memory_order_relaxed);
        } else if (staged == camera::TransactionResult::noCurrentListener ||
                   staged == camera::TransactionResult::listenerHidden ||
                   staged == camera::TransactionResult::listenerMissingCamera ||
                   staged == camera::TransactionResult::listenerMissingWorld ||
                   staged == camera::TransactionResult::unsupportedRotateMode ||
                   staged == camera::TransactionResult::clockUnavailable ||
                   staged == camera::TransactionResult::snapshotsMissing ||
                   staged == camera::TransactionResult::identityChanged) {
            g_cameraInterpolationSkipped.fetch_add(1,
                                                   std::memory_order_relaxed);
        } else {
            g_cameraInterpolationFailures.fetch_add(1,
                                                    std::memory_order_relaxed);
        }
    } else if (g_cameraRestoreProbe) {
        staged = g_cameraRuntime.BeginIdentityProbe(scene);
        if (staged == camera::TransactionResult::staged) {
            g_cameraProbeStaged.fetch_add(1, std::memory_order_relaxed);
        } else if (staged == camera::TransactionResult::noCurrentListener ||
                   staged == camera::TransactionResult::listenerHidden ||
                   staged == camera::TransactionResult::listenerMissingCamera ||
                   staged == camera::TransactionResult::listenerMissingWorld ||
                   staged == camera::TransactionResult::unsupportedRotateMode) {
            g_cameraProbeSkipped.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cameraProbeFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    BeginHandsIdentityProbe();

    if (g_worldTraceEnabled) {
        world::BeginRender(
            g_fixedSerial.load(std::memory_order_relaxed),
            g_hasActiveHandsClockSample ? &g_activeHandsClockSample : nullptr,
            g_worldRootInterpolation,
            g_viewLeadActive ? &g_viewLead : nullptr);
    }
    g_handsPoseRenderActive = g_handsTraceEnabled;
    if (g_particleInterpolation) {
        particle_runtime::BeginRender(g_hasActiveHandsClockSample,
                                      g_activeHandsClockSample.newerSerial,
                                      g_activeHandsClockSample.alpha);
    }
    frame_trace::Mark(frame_trace::Stamp::renderOriginal);
    g_originalSceneRenderAll(scene, frameTime, renderFlags, renderTime,
                             viewportMask);
    frame_trace::Mark(frame_trace::Stamp::renderEnd);
    if (g_particleInterpolation) {
        particle_runtime::EndRender();
    }
    g_handsPoseRenderActive = false;
    if (g_worldTraceEnabled) {
        world::EndRender();
    }
    g_viewLeadActive = false;
    if (g_lightFrustumsToRestoreCount != 0) {
        RestoreLightFrustums();
    }
    g_hasActiveHandsClockSample = false;
    RestoreHandsIdentityProbe();
    if (!g_cameraRuntime.IsActive()) {
        return;
    }
    const camera::TransactionResult restored = g_cameraRuntime.Restore();
    if (g_cameraInterpolation && restored == camera::TransactionResult::restored) {
        g_cameraInterpolationRestored.fetch_add(1,
                                                std::memory_order_relaxed);
    } else if (g_cameraInterpolation) {
        g_cameraInterpolationFailures.fetch_add(1,
                                                std::memory_order_relaxed);
    } else if (restored == camera::TransactionResult::restored) {
        g_cameraProbeRestored.fetch_add(1, std::memory_order_relaxed);
    } else if (restored == camera::TransactionResult::identityChanged) {
        g_cameraProbeIdentityChanges.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_cameraProbeFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

bool Install(const mapping::TelemetryMap& map,
             const Options& options,
             std::wstring& error) noexcept {
    error.clear();
    if (map.steadySwapWrapper == 0 || map.logicTimerWantUpdate == 0 ||
        map.limitFpsGetter == 0 || map.engineGlobalPointer == 0 ||
        (options.telemetryEnabled &&
         (map.updaterDispatch == 0 || map.engineMicrosecondClock == 0)) ||
        (options.motionTraceEnabled &&
         (!options.telemetryEnabled || map.sceneGlobalPointer == 0)) ||
        (options.cameraRestoreProbe &&
         (!options.telemetryEnabled || !options.motionTraceEnabled ||
          map.sceneRenderAll == 0)) ||
        (options.cameraInterpolation &&
         (!options.telemetryEnabled || !options.renderUnlock ||
          map.sceneGlobalPointer == 0 || map.sceneRenderAll == 0)) ||
        (options.handsTraceEnabled &&
         (!options.telemetryEnabled || !options.motionTraceEnabled ||
          map.sceneGlobalPointer == 0 || map.sceneRenderAll == 0 ||
          map.entitySetMatrix == 0 || map.entityUpdateAttachment == 0 ||
          map.subMeshGetModelMatrix == 0 ||
          map.subMeshGetSkinningPalette == 0 ||
          map.subMeshUpdateSkinning == 0)) ||
        (options.handsRenderCopyProbe && !options.handsTraceEnabled) ||
        (options.handsPoseRestoreProbe &&
         (!options.handsTraceEnabled || map.meshUpdatePalette == 0)) ||
        (options.handsPoseRenderCopyProbe && !options.handsTraceEnabled) ||
        (options.handsPoseInterpolation &&
         (!options.handsTraceEnabled || !options.handsRootInterpolation ||
          !options.cameraInterpolation)) ||
        (options.handsRootInterpolation &&
         (!options.handsTraceEnabled || !options.cameraInterpolation)) ||
        (options.handsRenderCopyProbe && options.handsRootInterpolation) ||
        (options.handsPoseRenderCopyProbe &&
         (options.handsPoseRestoreProbe || options.handsPoseInterpolation)) ||
        options.handsRestoreProbe ||
        (options.gameplayRateOverride != 0 &&
         ((options.gameplayRateOverride != 60 &&
           options.gameplayRateOverride != 120) ||
          !options.renderUnlock || map.engineSetUpdatesPerSec == 0)) ||
        (options.worldPoseInterpolation && !options.worldRootInterpolation) ||
        (options.mouseTraceEnabled &&
         (!options.telemetryEnabled || !options.motionTraceEnabled)) ||
        (options.frameTraceEnabled && !options.telemetryEnabled) ||
        (options.particleInterpolation &&
         (!options.handsRootInterpolation ||
          map.particleUpdateGraphics == 0)) ||
        (options.mouseOverlay &&
         (!options.mouseTraceEnabled || map.imGuiDraw == 0 ||
          map.imGuiDoMouse == 0 || map.imGuiEnd == 0 ||
          map.imGuiSendMousePosition == 0 ||
          map.imGuiSendMouseVirtualPosition == 0)) ||
        (options.mousePreview &&
         (!options.mouseTraceEnabled || !options.cameraInterpolation ||
          !options.handsRootInterpolation ||
          !options.worldRootInterpolation ||
          map.cameraGetViewMatrix == 0)) ||
        (options.worldTraceEnabled && !options.handsTraceEnabled) ||
        (options.worldRootInterpolation &&
         (!options.worldTraceEnabled || !options.handsRootInterpolation ||
          !options.cameraInterpolation ||
          map.renderableGetMatrixUpdateCount == 0 ||
          map.lightSpotGetViewProjMatrix == 0 ||
          map.lightSpotGetProjectionMatrix == 0 ||
          map.lightProjectionBiasMatrix == 0 ||
          map.shadowMapNeedsUpdate == 0 ||
          map.lightSpotGetViewMatrix == 0 ||
          map.lightSpotGetFrustum == 0)) ||
        (options.cameraRestoreProbe && options.cameraInterpolation) ||
        (!options.telemetryEnabled && !options.renderUnlock &&
         !options.presentationLimiter) ||
        (options.telemetryEnabled && options.reportIntervalSeconds == 0) ||
        (options.presentationLimiter &&
         (!options.renderUnlock || options.targetFps == 0))) {
        error = L"Runtime hook configuration is invalid";
        return false;
    }
    g_telemetryEnabled = options.telemetryEnabled;
    g_motionTraceEnabled = options.motionTraceEnabled;
    g_cameraRestoreProbe = options.cameraRestoreProbe;
    g_cameraInterpolation = options.cameraInterpolation;
    g_handsTraceEnabled = options.handsTraceEnabled;
    g_handsRenderCopyProbe = options.handsRenderCopyProbe;
    g_handsPoseRestoreProbe = options.handsPoseRestoreProbe;
    g_handsPoseRenderCopyProbe = options.handsPoseRenderCopyProbe;
    g_handsRootInterpolation = options.handsRootInterpolation;
    g_handsPoseInterpolation = options.handsPoseInterpolation;
    g_handsRestoreProbe = options.handsRestoreProbe;
    g_worldTraceEnabled = options.worldTraceEnabled;
    g_worldRootInterpolation = options.worldRootInterpolation;
    world::ConfigurePose(options.worldPoseInterpolation);
    g_mouseTraceEnabled = options.mouseTraceEnabled;
    g_mousePreview = options.mousePreview;
    g_cameraGetViewMatrix =
        reinterpret_cast<CameraGetViewMatrix>(map.cameraGetViewMatrix);
    g_renderUnlock = options.renderUnlock;
    g_presentationLimiter = options.presentationLimiter;
    g_engineGlobalPointer = map.engineGlobalPointer;
    g_sceneGlobalPointer = map.sceneGlobalPointer;
    g_engineMicrosecondClock = reinterpret_cast<EngineMicrosecondClock>(
        map.engineMicrosecondClock);
    g_engineSetUpdatesPerSec = reinterpret_cast<EngineSetUpdatesPerSec>(
        map.engineSetUpdatesPerSec);
    g_gameplayRateOverride = static_cast<int>(options.gameplayRateOverride);
    g_lightSpotGetProjectionMatrix = reinterpret_cast<LightSpotGetMatrix>(
        map.lightSpotGetProjectionMatrix);
    g_lightProjectionBias = reinterpret_cast<const hands_trace::Matrix*>(
        map.lightProjectionBiasMatrix);
    if (options.telemetryEnabled || options.presentationLimiter) {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            error = L"QueryPerformanceFrequency failed";
            return false;
        }
        g_qpcFrequency = frequency.QuadPart;
    }
    if (options.telemetryEnabled) {
        g_reportIntervalTicks = g_qpcFrequency *
            static_cast<std::int64_t>(options.reportIntervalSeconds);
    }
    if (options.presentationLimiter) {
        if (!g_deadlineSchedule.Configure(g_qpcFrequency, options.targetFps)) {
            error = L"Presentation limiter schedule configuration failed";
            return false;
        }
        g_spinThresholdTicks = std::max<std::int64_t>(g_qpcFrequency / 2000, 1);
        g_evenFramePacing =
            options.framePacingEvenness != 0 &&
            g_evenPacer.Configure(
                g_qpcFrequency,
                g_qpcFrequency / static_cast<std::int64_t>(options.targetFps),
                options.framePacingEvenness);
        g_waitableTimer = CreateWaitableTimerExW(
            nullptr, nullptr, kCreateWaitableTimerHighResolution,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        g_highResolutionTimer = g_waitableTimer != nullptr;
        if (g_waitableTimer == nullptr) {
            g_waitableTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        if (g_waitableTimer == nullptr) {
            error = L"Could not create the presentation limiter timer (Win32 " +
                std::to_wstring(GetLastError()) + L")";
            return false;
        }
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        error = MinHookError(L"MH_Initialize", status);
        return false;
    }
    void* const target = reinterpret_cast<void*>(map.steadySwapWrapper);
    void* const timerTarget =
        reinterpret_cast<void*>(map.logicTimerWantUpdate);
    void* const updaterTarget =
        reinterpret_cast<void*>(map.updaterDispatch);
    void* const sceneRenderTarget =
        reinterpret_cast<void*>(map.sceneRenderAll);
    void* const entitySetMatrixTarget =
        reinterpret_cast<void*>(map.entitySetMatrix);
    void* const entityAttachmentTarget =
        reinterpret_cast<void*>(map.entityUpdateAttachment);
    void* const subMeshModelTarget =
        reinterpret_cast<void*>(map.subMeshGetModelMatrix);
    void* const subMeshPaletteGetterTarget =
        reinterpret_cast<void*>(map.subMeshGetSkinningPalette);
    void* const subMeshSkinningTarget =
        reinterpret_cast<void*>(map.subMeshUpdateSkinning);
    void* const meshPaletteTarget =
        reinterpret_cast<void*>(map.meshUpdatePalette);
    const bool hookHandsPose = options.handsPoseRestoreProbe;
    const bool hookSceneRender = options.cameraRestoreProbe ||
                                 options.cameraInterpolation ||
                                 options.handsTraceEnabled;
    status = MH_CreateHook(target, reinterpret_cast<void*>(&HookedSteadySwap),
                           reinterpret_cast<void**>(&g_originalSwap));
    if (status != MH_OK) {
        error = MinHookError(L"MH_CreateHook(steady swap)", status);
        return false;
    }
    if (options.telemetryEnabled) {
        status = MH_CreateHook(timerTarget,
                               reinterpret_cast<void*>(&HookedLogicTimerWantUpdate),
                               reinterpret_cast<void**>(&g_originalWantUpdate));
        if (status != MH_OK) {
            (void)MH_RemoveHook(target);
            g_originalSwap = nullptr;
            error = MinHookError(L"MH_CreateHook(logic timer WantUpdate)", status);
            return false;
        }
        status = MH_CreateHook(updaterTarget,
                               reinterpret_cast<void*>(&HookedUpdaterDispatch),
                               reinterpret_cast<void**>(&g_originalUpdaterDispatch));
        if (status != MH_OK) {
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(L"MH_CreateHook(updater dispatch)", status);
            return false;
        }
    }
    if (hookSceneRender) {
        status = MH_CreateHook(
            sceneRenderTarget,
            reinterpret_cast<void*>(&HookedSceneRenderAll),
            reinterpret_cast<void**>(&g_originalSceneRenderAll));
        if (status != MH_OK) {
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(L"MH_CreateHook(scene RenderAll)", status);
            return false;
        }
    }
    if (options.handsTraceEnabled) {
        status = MH_CreateHook(
            entitySetMatrixTarget,
            reinterpret_cast<void*>(&HookedEntitySetMatrix),
            reinterpret_cast<void**>(&g_originalEntitySetMatrix));
        if (status != MH_OK) {
            (void)MH_RemoveHook(sceneRenderTarget);
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalSceneRenderAll = nullptr;
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(L"MH_CreateHook(iLuxEntity::SetMatrix)", status);
            return false;
        }
        status = MH_CreateHook(
            entityAttachmentTarget,
            reinterpret_cast<void*>(&HookedEntityUpdateAttachment),
            reinterpret_cast<void**>(&g_originalEntityUpdateAttachment));
        if (status != MH_OK) {
            (void)MH_RemoveHook(entitySetMatrixTarget);
            (void)MH_RemoveHook(sceneRenderTarget);
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalEntitySetMatrix = nullptr;
            g_originalSceneRenderAll = nullptr;
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(
                L"MH_CreateHook(iLuxEntity::UpdateEntityAttachment)", status);
            return false;
        }
        status = MH_CreateHook(
            subMeshModelTarget,
            reinterpret_cast<void*>(&HookedSubMeshGetModelMatrix),
            reinterpret_cast<void**>(&g_originalSubMeshGetModelMatrix));
        if (status != MH_OK) {
            (void)MH_RemoveHook(entityAttachmentTarget);
            (void)MH_RemoveHook(entitySetMatrixTarget);
            (void)MH_RemoveHook(sceneRenderTarget);
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalEntityUpdateAttachment = nullptr;
            g_originalEntitySetMatrix = nullptr;
            g_originalSceneRenderAll = nullptr;
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(
                L"MH_CreateHook(cSubMeshEntity::GetModelMatrix)", status);
            return false;
        }
        status = MH_CreateHook(
            subMeshPaletteGetterTarget,
            reinterpret_cast<void*>(&HookedSubMeshGetSkinningPalette),
            reinterpret_cast<void**>(&g_originalSubMeshGetSkinningPalette));
        if (status != MH_OK) {
            (void)MH_RemoveHook(subMeshModelTarget);
            (void)MH_RemoveHook(entityAttachmentTarget);
            (void)MH_RemoveHook(entitySetMatrixTarget);
            (void)MH_RemoveHook(sceneRenderTarget);
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalSubMeshGetModelMatrix = nullptr;
            g_originalEntityUpdateAttachment = nullptr;
            g_originalEntitySetMatrix = nullptr;
            g_originalSceneRenderAll = nullptr;
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(
                L"MH_CreateHook(cSubMeshEntity::GetSkinningPalette)", status);
            return false;
        }
        status = MH_CreateHook(
            subMeshSkinningTarget,
            reinterpret_cast<void*>(&HookedSubMeshUpdateSkinning),
            reinterpret_cast<void**>(&g_originalSubMeshUpdateSkinning));
        if (status != MH_OK) {
            (void)MH_RemoveHook(subMeshPaletteGetterTarget);
            (void)MH_RemoveHook(subMeshModelTarget);
            (void)MH_RemoveHook(entityAttachmentTarget);
            (void)MH_RemoveHook(entitySetMatrixTarget);
            (void)MH_RemoveHook(sceneRenderTarget);
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
            (void)MH_RemoveHook(target);
            g_originalSubMeshGetModelMatrix = nullptr;
            g_originalSubMeshGetSkinningPalette = nullptr;
            g_originalEntityUpdateAttachment = nullptr;
            g_originalEntitySetMatrix = nullptr;
            g_originalSceneRenderAll = nullptr;
            g_originalUpdaterDispatch = nullptr;
            g_originalWantUpdate = nullptr;
            g_originalSwap = nullptr;
            error = MinHookError(
                L"MH_CreateHook(cSubMeshEntity skinning update)", status);
            return false;
        }
        if (hookHandsPose) {
            status = MH_CreateHook(
                meshPaletteTarget,
                reinterpret_cast<void*>(&HookedMeshUpdatePalette),
                reinterpret_cast<void**>(&g_originalMeshUpdatePalette));
            if (status != MH_OK) {
                (void)MH_RemoveHook(subMeshSkinningTarget);
                (void)MH_RemoveHook(subMeshPaletteGetterTarget);
                (void)MH_RemoveHook(subMeshModelTarget);
                (void)MH_RemoveHook(entityAttachmentTarget);
                (void)MH_RemoveHook(entitySetMatrixTarget);
                (void)MH_RemoveHook(sceneRenderTarget);
                (void)MH_RemoveHook(updaterTarget);
                (void)MH_RemoveHook(timerTarget);
                (void)MH_RemoveHook(target);
                g_originalSubMeshUpdateSkinning = nullptr;
                g_originalSubMeshGetSkinningPalette = nullptr;
                g_originalSubMeshGetModelMatrix = nullptr;
                g_originalEntityUpdateAttachment = nullptr;
                g_originalEntitySetMatrix = nullptr;
                g_originalSceneRenderAll = nullptr;
                g_originalUpdaterDispatch = nullptr;
                g_originalWantUpdate = nullptr;
                g_originalSwap = nullptr;
                error = MinHookError(
                    L"MH_CreateHook(cMeshEntity palette update)", status);
                return false;
            }
        }
    }
    void* const matrixUpdateCountTarget =
        reinterpret_cast<void*>(map.renderableGetMatrixUpdateCount);
    const bool hookMatrixUpdateCount = options.worldRootInterpolation;
    bool matrixUpdateCountCreated = false;
    status = MH_OK;
    if (hookMatrixUpdateCount) {
        status = MH_CreateHook(
            matrixUpdateCountTarget,
            reinterpret_cast<void*>(&HookedRenderableGetMatrixUpdateCount),
            reinterpret_cast<void**>(
                &g_originalRenderableGetMatrixUpdateCount));
        matrixUpdateCountCreated = status == MH_OK;
    }
    void* const lightViewProjTarget =
        reinterpret_cast<void*>(map.lightSpotGetViewProjMatrix);
    bool lightViewProjCreated = false;
    if (status == MH_OK && hookMatrixUpdateCount) {
        status = MH_CreateHook(
            lightViewProjTarget,
            reinterpret_cast<void*>(&HookedLightSpotGetViewProjMatrix),
            reinterpret_cast<void**>(&g_originalLightSpotGetViewProjMatrix));
        lightViewProjCreated = status == MH_OK;
    }
    void* const shadowNeedsUpdateTarget =
        reinterpret_cast<void*>(map.shadowMapNeedsUpdate);
    bool shadowNeedsUpdateCreated = false;
    if (status == MH_OK && hookMatrixUpdateCount) {
        status = MH_CreateHook(
            shadowNeedsUpdateTarget,
            reinterpret_cast<void*>(&HookedShadowMapNeedsUpdate),
            reinterpret_cast<void**>(&g_originalShadowMapNeedsUpdate));
        shadowNeedsUpdateCreated = status == MH_OK;
    }
    void* const lightViewTarget =
        reinterpret_cast<void*>(map.lightSpotGetViewMatrix);
    void* const lightFrustumTarget =
        reinterpret_cast<void*>(map.lightSpotGetFrustum);
    bool lightViewCreated = false;
    bool lightFrustumCreated = false;
    if (status == MH_OK && hookMatrixUpdateCount) {
        status = MH_CreateHook(
            lightViewTarget,
            reinterpret_cast<void*>(&HookedLightSpotGetViewMatrix),
            reinterpret_cast<void**>(&g_originalLightSpotGetViewMatrix));
        lightViewCreated = status == MH_OK;
    }
    if (status == MH_OK && hookMatrixUpdateCount) {
        status = MH_CreateHook(
            lightFrustumTarget,
            reinterpret_cast<void*>(&HookedLightSpotGetFrustum),
            reinterpret_cast<void**>(&g_originalLightSpotGetFrustum));
        lightFrustumCreated = status == MH_OK;
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(target);
    }
    if (status == MH_OK && matrixUpdateCountCreated) {
        status = MH_QueueEnableHook(matrixUpdateCountTarget);
    }
    if (status == MH_OK && lightViewProjCreated) {
        status = MH_QueueEnableHook(lightViewProjTarget);
    }
    if (status == MH_OK && shadowNeedsUpdateCreated) {
        status = MH_QueueEnableHook(shadowNeedsUpdateTarget);
    }
    if (status == MH_OK && lightViewCreated) {
        status = MH_QueueEnableHook(lightViewTarget);
    }
    if (status == MH_OK && lightFrustumCreated) {
        status = MH_QueueEnableHook(lightFrustumTarget);
    }
    if (status == MH_OK && options.telemetryEnabled) {
        status = MH_QueueEnableHook(timerTarget);
    }
    if (status == MH_OK && options.telemetryEnabled) {
        status = MH_QueueEnableHook(updaterTarget);
    }
    if (status == MH_OK &&
        hookSceneRender) {
        status = MH_QueueEnableHook(sceneRenderTarget);
    }
    if (status == MH_OK && options.handsTraceEnabled) {
        status = MH_QueueEnableHook(entitySetMatrixTarget);
    }
    if (status == MH_OK && options.handsTraceEnabled) {
        status = MH_QueueEnableHook(entityAttachmentTarget);
    }
    if (status == MH_OK && options.handsTraceEnabled) {
        status = MH_QueueEnableHook(subMeshModelTarget);
    }
    if (status == MH_OK && options.handsTraceEnabled) {
        status = MH_QueueEnableHook(subMeshPaletteGetterTarget);
    }
    if (status == MH_OK && options.handsTraceEnabled) {
        status = MH_QueueEnableHook(subMeshSkinningTarget);
    }
    if (status == MH_OK && hookHandsPose) {
        status = MH_QueueEnableHook(meshPaletteTarget);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        if (matrixUpdateCountCreated) {
            (void)MH_RemoveHook(matrixUpdateCountTarget);
            g_originalRenderableGetMatrixUpdateCount = nullptr;
        }
        if (lightViewProjCreated) {
            (void)MH_RemoveHook(lightViewProjTarget);
            g_originalLightSpotGetViewProjMatrix = nullptr;
        }
        if (shadowNeedsUpdateCreated) {
            (void)MH_RemoveHook(shadowNeedsUpdateTarget);
            g_originalShadowMapNeedsUpdate = nullptr;
        }
        if (lightViewCreated) {
            (void)MH_RemoveHook(lightViewTarget);
            g_originalLightSpotGetViewMatrix = nullptr;
        }
        if (lightFrustumCreated) {
            (void)MH_RemoveHook(lightFrustumTarget);
            g_originalLightSpotGetFrustum = nullptr;
        }
        if (options.handsTraceEnabled) {
            if (hookHandsPose) {
                (void)MH_RemoveHook(meshPaletteTarget);
            }
            (void)MH_RemoveHook(subMeshSkinningTarget);
            (void)MH_RemoveHook(subMeshPaletteGetterTarget);
            (void)MH_RemoveHook(subMeshModelTarget);
            (void)MH_RemoveHook(entityAttachmentTarget);
            (void)MH_RemoveHook(entitySetMatrixTarget);
        }
        if (hookSceneRender) {
            (void)MH_RemoveHook(sceneRenderTarget);
        }
        if (options.telemetryEnabled) {
            (void)MH_RemoveHook(updaterTarget);
            (void)MH_RemoveHook(timerTarget);
        }
        (void)MH_RemoveHook(target);
        g_originalWantUpdate = nullptr;
        g_originalUpdaterDispatch = nullptr;
        g_originalSceneRenderAll = nullptr;
        g_originalEntitySetMatrix = nullptr;
        g_originalEntityUpdateAttachment = nullptr;
        g_originalSubMeshGetModelMatrix = nullptr;
        g_originalSubMeshGetSkinningPalette = nullptr;
        g_originalSubMeshUpdateSkinning = nullptr;
        g_originalMeshUpdatePalette = nullptr;
        g_originalSwap = nullptr;
        error = MinHookError(L"Enabling telemetry hooks", status);
        return false;
    }
    if (options.frameTraceEnabled) {
        std::wstring frameError;
        if (frame_trace::Start(g_qpcFrequency, frameError)) {
            log::Write(log::Level::info,
                       L"Frame trace armed: one record per presented frame "
                       L"is written to the plugin's .frames.bin by a "
                       L"background thread.");
        } else {
            log::Write(log::Level::warning,
                       L"Frame trace unavailable: " + frameError);
        }
    }
    if (options.mouseTraceEnabled) {
        std::wstring mouseError;
        if (mouse::InstallTrace(mouseError)) {
            mouse::ConfigurePreview(g_mousePreview);
            log::Write(log::Level::info,
                       g_mousePreview
                           ? L"MousePreview armed: the presented view leads "
                             L"the interpolated camera by mouse motion the "
                             L"game has not shown yet. Scale, limits, and "
                             L"axis convention are learned from the game's "
                             L"own response; no input is consumed, "
                             L"reordered, or injected."
                           : L"Read-only mouse trace armed: relative motion "
                             L"is observed through an SDL event watch and "
                             L"the game's SDL_GetRelativeMouseState calls "
                             L"are counted; no input is consumed or "
                             L"altered.");
        } else {
            // Without the trace there is no preview either; the proven
            // layers stay active.
            g_mouseTraceEnabled = false;
            g_mousePreview = false;
            log::Write(log::Level::warning,
                       L"Mouse trace unavailable: " + mouseError);
        }
    }
    if (options.particleInterpolation) {
        std::wstring particleError;
        if (particle_runtime::Install(map.particleUpdateGraphics,
                                      particleError)) {
            g_particleInterpolation = true;
            log::Write(log::Level::info,
                       L"ParticleInterpolation armed at the emitter's "
                       L"per-view graphics update (RVA 0x005A6080): "
                       L"particles of emitters simulated in the presented "
                       L"tick are drawn between their last two positions "
                       L"and restored afterwards.");
        } else {
            // The proven layers stay active without it.
            log::Write(log::Level::warning,
                       L"Particle interpolation unavailable: " +
                           particleError);
        }
    }
    if (options.mouseOverlay && g_mouseTraceEnabled) {
        std::wstring guiError;
        const gui_cursor::Targets targets{
            map.imGuiDraw, map.imGuiDoMouse, map.imGuiEnd,
            map.imGuiSendMousePosition, map.imGuiSendMouseVirtualPosition};
        if (gui_cursor::Install(targets, guiError)) {
            g_guiCursor = true;
            log::Write(log::Level::info,
                       L"MouseOverlay armed: the menu and inventory cursor "
                       L"is moved to the current mouse position around each "
                       L"replay of cImGui's draw list (RVA 0x0047C550) and "
                       L"put back afterwards; hover and clicks stay on the "
                       L"tick.");
        } else {
            // The proven layers stay active without it.
            log::Write(log::Level::warning,
                       L"GUI cursor unavailable: " + guiError);
        }
    }
    if (options.telemetryEnabled) {
        log::Write(log::Level::info,
                   L"Telemetry installed at steady swap RVA 0x005EC4F0, logic "
                   L"timer WantUpdate RVA 0x00555600, and updater dispatch RVA "
                   L"0x0039E900; scheduler phase and LimitFPS use "
                   L"signature-validated mappings.");
    }
    if (options.motionTraceEnabled) {
        log::Write(log::Level::info,
                   L"Read-only camera motion trace armed: scene current listener "
                   L"and camera transforms will be sampled after completed fixed "
                   L"updates and at presentation; camera state will not be modified.");
    }
    if (options.cameraRestoreProbe) {
        log::Write(log::Level::info,
                   L"CameraRestoreProbe armed around scene RenderAll RVA "
                   L"0x00424B20: the current-listener pose is staged unchanged, "
                   L"then proven camera caches are restored and byte-verified.");
    }
    if (options.cameraInterpolation) {
        log::Write(log::Level::info,
                   L"CameraInterpolation armed at the proven "
                   L"_PostVariableUpdate/RenderAll boundaries; non-Euler, "
                   L"missing, discontinuous, and unavailable states skip "
                   L"without modifying the camera.");
    }
    if (options.handsTraceEnabled) {
        log::Write(log::Level::info,
                   L"Read-only hands/equipment trace armed at "
                   L"iLuxEntity::SetMatrix RVA 0x001988E0 and "
                   L"UpdateEntityAttachment RVA 0x0019BE50, with a pass-through "
                   L"cSubMeshEntity::GetModelMatrix hook at RVA 0x004460A0 and "
                   L"a pass-through renderer dual-quaternion palette getter at "
                   L"RVA 0x004ADAC0, plus the post-call matrix-palette trace at "
                   L"RVA 0x004ADEB0; renderer-owned objects and both palette "
                   L"representations are observed without modifying state.");
    }
    if (options.handsRenderCopyProbe) {
        log::Write(log::Level::info,
                   L"HandsRenderCopyProbe armed: near-camera first-person "
                   L"submeshes receive a byte-exact matrix copy from "
                   L"plugin-owned aligned storage; no game matrix or object "
                   L"is modified.");
    }
    if (options.handsRootInterpolation) {
        log::Write(log::Level::info,
                   L"HandsRootInterpolation armed for renderer-matched "
                   L"mesh owners within two metres: consecutive fixed-update "
                   L"roots use the proven presentation clock and return an "
                   L"interpolated plugin-owned matrix without modifying game "
                   L"state.");
    }
    if (options.handsPoseRestoreProbe) {
        log::Write(log::Level::info,
                   L"HandsPoseRestoreProbe armed at cMeshEntity palette "
                   L"update RVA 0x00334400: the proven 23-bone first-person "
                   L"palette is reapplied byte-identically only inside its "
                   L"live submesh callback, then restored and verified.");
    }
    if (options.handsPoseRenderCopyProbe) {
        log::Write(log::Level::info,
                   L"HandsPoseRenderCopyProbe armed at the renderer-facing "
                   L"dual-quaternion getter RVA 0x004ADAC0: only the proven "
                   L"near 23-bone palette is returned from byte-identical "
                   L"plugin-owned storage; game state is not written.");
    }
    if (options.handsPoseInterpolation) {
        log::Write(log::Level::info,
                   L"HandsPoseInterpolation armed for first-person "
                   L"renderer palettes (held-item rig and player body, up "
                   L"to 128 bones): completed-tick dual-quaternion pairs are "
                   L"sampled on the shared presentation clock and returned "
                   L"from plugin-owned per-owner storage without writing "
                   L"game state");
    }
    if (options.worldTraceEnabled) {
        log::Write(log::Level::info,
                   options.worldRootInterpolation
                       ? L"WorldRootInterpolation armed: submeshes whose "
                         L"model matrix changes across adjacent fixed "
                         L"ticks receive an interpolated plugin-owned "
                         L"matrix; history is built only inside the live "
                         L"renderer callback and no game state is written."
                       : L"Read-only world-object trace armed: per-submesh "
                         L"model-matrix history is built inside the live "
                         L"renderer callback; every engine pointer is "
                         L"returned unchanged.");
    }
    if (options.worldPoseInterpolation) {
        log::Write(log::Level::info,
                   L"WorldPoseInterpolation armed: skinned world meshes "
                   L"whose renderer palette changes across adjacent fixed "
                   L"ticks receive an interpolated plugin-owned palette of "
                   L"up to 256 bones; no game state is written.");
    }
    if (options.handsRestoreProbe) {
        log::Write(log::Level::info,
                   L"HandsRestoreProbe armed around scene RenderAll: fresh "
                   L"hands/equipment matrices will be reapplied unchanged, "
                   L"restored in reverse order, and byte-verified.");
    }
    if (options.renderUnlock) {
        log::Write(log::Level::info,
                   options.gameplayRateOverride == 0
                       ? L"RenderUnlock armed on the steady swap boundary; "
                         L"gameplay update-rate state will not be modified."
                       : L"RenderUnlock armed on the steady swap boundary.");
    }
    if (options.gameplayRateOverride != 0) {
        log::Write(log::Level::info,
                   L"GameplayRateOverride armed: the authoritative update "
                   L"rate will be held at " +
                       std::to_wstring(options.gameplayRateOverride) +
                       L" Hz through the engine's own setter (opt-in).");
    }
    if (options.presentationLimiter) {
        log::Write(log::Level::info,
                   L"PresentationLimiter armed at " +
                       std::to_wstring(options.targetFps) + L" FPS using a " +
                       (g_highResolutionTimer ? L"high-resolution" : L"standard") +
                       L" waitable timer plus a short QPC precision wait.");
    }
    if (g_evenFramePacing) {
        log::Write(log::Level::info,
                   L"Frame pacing armed at evenness " +
                       std::to_wstring(options.framePacingEvenness) +
                       L": one tick frame and a whole number of equal plain "
                       L"frames per simulation tick, locked to the engine's "
                       L"logic timer and never faster than TargetFPS; the "
                       L"plain cap is used whenever that timer is unusable.");
    }
    return true;
}

} // namespace atb::telemetry
