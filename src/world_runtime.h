#pragma once

#include "hands_trace.h"
#include "presentation_clock.h"

#include <cstddef>
#include <cstdint>

// Render-time interpolation of world-object model matrices.
//
// Each submesh's two-tick history is built entirely inside the renderer's own
// model-matrix callback, where the submesh is live. Nothing is captured during
// the fixed update and no engine pointer is dereferenced later: a submesh
// address is only ever used as an opaque table key.
namespace atb::world {

struct Counters {
    std::uint64_t tracked = 0;         // first sight of a submesh in a render
    std::uint64_t moving = 0;          // changed across adjacent fixed ticks
    std::uint64_t eligible = 0;        // moving and a valid interpolant exists
    std::uint64_t applied = 0;         // interpolant handed to the renderer
    std::uint64_t unavailable = 0;     // moving, but no matching clock pair
    std::uint64_t rejected = 0;        // moving, but not rigid or discontinuous
    std::uint64_t invalid = 0;         // non-finite or non-affine source
    std::uint64_t sameTickChanges = 0; // changed without a fixed tick
    std::uint64_t frameVariants = 0;   // differed between calls in one render
    std::uint64_t quarantined = 0;     // moving while quarantined
    std::uint64_t tableFull = 0;       // no slot within the probe bound
    std::uint64_t evictions = 0;       // stale slot reused
    std::uint64_t updateCountRefreshes = 0; // perturbed update counts
    std::uint64_t led = 0;             // applied with the view's look lead
};

// Skinned world meshes (enemies, corpses, animated props). The same
// render-time approach as the model matrices: a two-tick history of each
// mesh's renderer-facing dual-quaternion palette, built inside the live
// palette getter and keyed by the mesh address as an opaque value.
struct PoseCounters {
    std::uint64_t tracked = 0;         // first sight of a mesh in a render
    std::uint64_t changed = 0;         // palette differs across adjacent ticks
    std::uint64_t applied = 0;         // interpolated palette handed out
    std::uint64_t unavailable = 0;     // changed, but no matching clock pair
    std::uint64_t rejected = 0;        // changed, but non-finite or a full cut
    std::uint64_t snappedBones = 0;    // bones left unblended in applied poses
    std::uint64_t sameTickChanges = 0; // palette changed without a fixed tick
    std::uint64_t quarantined = 0;     // changed while quarantined
    std::uint64_t oversize = 0;        // more bones than the history holds
    std::uint64_t tableFull = 0;
    std::uint64_t maximumBones = 0;    // largest palette seen this interval
    std::uint64_t changedMaximumBones = 0; // largest animated palette
};

// `substitute` false keeps the history and counters but always returns the
// engine's palette.
void ConfigurePose(bool substitute) noexcept;

// Safe from any thread while a render is active. `palette` is the live
// renderer-facing dual-quaternion array of `boneCount` 32-byte entries.
// Returns `palette` or plugin-owned storage that stays unchanged until this
// mesh is next seen in a later render.
[[nodiscard]] const void* PresentPalette(const void* mesh,
                                         const void* palette,
                                         std::size_t boneCount) noexcept;
[[nodiscard]] PoseCounters TakePoseCounters() noexcept;

// Extra rigid rotation about a point, used to keep things attached to the
// player's view in step with a camera that leads its interpolated pose.
struct LeadTransform {
    float rotation[9]{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    float center[3]{};
    // Moving world objects whose origin lies within this distance of the
    // centre are treated as carried with the view.
    float radius = 0.0F;
};
void ApplyLead(const LeadTransform& lead, hands_trace::Matrix& matrix) noexcept;

// Called on the render thread around one scene render. `sample` and `lead`
// may be null; `substitute` false builds history and counters only.
void BeginRender(std::uint64_t fixedSerial,
                 const clock::Sample* sample,
                 bool substitute,
                 const LeadTransform* lead = nullptr) noexcept;
void EndRender() noexcept;

// Safe from any thread while a render is active. Returns `original` or a
// pointer to plugin-owned storage that stays unchanged until this submesh is
// next seen in a later render.
[[nodiscard]] const hands_trace::Matrix* Present(
    const void* subMesh, const hands_trace::Matrix* original) noexcept;

// The renderer decides whether cached per-object results (notably shadow
// maps) are still valid by comparing each renderable's matrix update count,
// which only changes on a fixed tick. While a renderable is being presented
// at an interpolated position its count is made to differ every render, so
// those caches follow the interpolated matrix instead of the last tick.
// Lock-free and safe from any thread; returns `original` for everything else.
[[nodiscard]] int RefreshUpdateCount(const void* renderable,
                                     int original) noexcept;

// True while a render is active and `renderable` was presented at an
// interpolated position in this or the previous render. Lock-free.
[[nodiscard]] bool WasPresentedRecently(const void* renderable) noexcept;

// Registers a renderable whose interpolated matrix is supplied by another
// layer (the first-person owners), so its update count is refreshed too.
void MarkPresented(const void* renderable) noexcept;

[[nodiscard]] Counters TakeCounters() noexcept;
void Reset() noexcept;

// Rigid interpolation that tolerates a constant per-axis scale.
[[nodiscard]] bool InterpolateScaledRigid(const hands_trace::Matrix& older,
                                          const hands_trace::Matrix& newer,
                                          double alpha,
                                          hands_trace::Matrix& result) noexcept;

} // namespace atb::world
