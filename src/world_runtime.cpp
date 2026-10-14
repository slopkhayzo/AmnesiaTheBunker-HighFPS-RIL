#include "world_runtime.h"

#include "hands_pose_interpolation.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace atb::world {
namespace {

constexpr std::size_t kTableBits = 14;
constexpr std::size_t kTableSize = std::size_t{1} << kTableBits;
constexpr std::size_t kProbeLimit = 16;
// A slot not seen for this many renders belongs to a culled or destroyed
// submesh and may be reused.
constexpr std::uint64_t kStaleRenders = 480;
// Matrices that change without a fixed tick are already driven at render
// rate. Leave them alone for this many ticks after the last such change.
constexpr std::uint64_t kQuarantineTicks = 120;

// One cache line multiple per slot keeps neighbouring slots from sharing a
// line across render threads; the padding is intended.
#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) Entry {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uint64_t> lastRender{0};
    // Render in which an interpolant was last produced for this slot.
    std::atomic<std::uint64_t> presentedRender{0};
    std::atomic_flag busy;
    std::uint64_t currentSerial = 0;
    std::uint64_t previousSerial = 0;
    std::uint64_t quarantineUntil = 0;
    bool hasCurrent = false;
    bool hasPrevious = false;
    bool presentValid = false;
    hands_trace::Matrix current{};
    hands_trace::Matrix previous{};
    alignas(16) hands_trace::Matrix presented{};
};
#pragma warning(pop)

std::array<Entry, kTableSize> g_entries{};

std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_render{0};
std::uint64_t g_fixedSerial = 0;
clock::Sample g_sample{};
bool g_hasSample = false;
bool g_substitute = false;
LeadTransform g_lead{};
bool g_hasLead = false;

struct AtomicCounters {
    std::atomic<std::uint64_t> tracked{0};
    std::atomic<std::uint64_t> moving{0};
    std::atomic<std::uint64_t> eligible{0};
    std::atomic<std::uint64_t> applied{0};
    std::atomic<std::uint64_t> unavailable{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> invalid{0};
    std::atomic<std::uint64_t> sameTickChanges{0};
    std::atomic<std::uint64_t> frameVariants{0};
    std::atomic<std::uint64_t> quarantined{0};
    std::atomic<std::uint64_t> tableFull{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> updateCountRefreshes{0};
    std::atomic<std::uint64_t> led{0};
};

AtomicCounters g_counters;

void Count(std::atomic<std::uint64_t>& counter) noexcept {
    counter.fetch_add(1, std::memory_order_relaxed);
}

void Lock(Entry& entry) noexcept {
    while (entry.busy.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
}

void Unlock(Entry& entry) noexcept {
    entry.busy.clear(std::memory_order_release);
}

std::size_t SlotFor(const std::uintptr_t key) noexcept {
    return static_cast<std::size_t>(
        ((key >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - kTableBits));
}

void Clear(Entry& entry) noexcept {
    entry.currentSerial = 0;
    entry.previousSerial = 0;
    entry.quarantineUntil = 0;
    entry.hasCurrent = false;
    entry.hasPrevious = false;
    entry.presentValid = false;
}

// Returns the locked slot for `key`, claiming an empty or stale one if the
// key is not present within the probe bound.
Entry* Acquire(const std::uintptr_t key, const std::uint64_t render) noexcept {
    const std::size_t first = SlotFor(key);
    for (std::size_t probe = 0; probe < kProbeLimit; ++probe) {
        Entry& entry = g_entries[(first + probe) & (kTableSize - 1)];
        if (entry.key.load(std::memory_order_acquire) != key) {
            continue;
        }
        Lock(entry);
        if (entry.key.load(std::memory_order_relaxed) == key) {
            return &entry;
        }
        Unlock(entry);
    }
    for (std::size_t probe = 0; probe < kProbeLimit; ++probe) {
        Entry& entry = g_entries[(first + probe) & (kTableSize - 1)];
        const auto reusable = [&entry, render]() noexcept {
            const std::uint64_t seen =
                entry.lastRender.load(std::memory_order_relaxed);
            return entry.key.load(std::memory_order_relaxed) == 0 ||
                   (render > seen && render - seen > kStaleRenders);
        };
        if (!reusable() ||
            entry.busy.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        if (!reusable()) {
            Unlock(entry);
            continue;
        }
        if (entry.key.load(std::memory_order_relaxed) != 0) {
            Count(g_counters.evictions);
        }
        Clear(entry);
        entry.lastRender.store(0, std::memory_order_relaxed);
        entry.presentedRender.store(0, std::memory_order_relaxed);
        entry.key.store(key, std::memory_order_release);
        return &entry;
    }
    return nullptr;
}

float ColumnLength(const hands_trace::Matrix& matrix,
                   const std::size_t column) noexcept {
    const float x = matrix.values[column];
    const float y = matrix.values[4 + column];
    const float z = matrix.values[8 + column];
    return std::sqrt(x * x + y * y + z * z);
}

void ScaleColumn(hands_trace::Matrix& matrix,
                 const std::size_t column,
                 const float factor) noexcept {
    matrix.values[column] *= factor;
    matrix.values[4 + column] *= factor;
    matrix.values[8 + column] *= factor;
}

void Observe(Entry& entry, const hands_trace::Matrix& source) noexcept {
    const std::uint64_t serial = g_fixedSerial;
    Count(g_counters.tracked);
    entry.presentValid = false;
    if (!hands_trace::IsFiniteAffine(source)) {
        Count(g_counters.invalid);
        Clear(entry);
        return;
    }
    if (!entry.hasCurrent || serial < entry.currentSerial) {
        entry.hasPrevious = false;
    } else if (serial == entry.currentSerial) {
        if (hands_trace::SameMatrix(source, entry.current)) {
            // Second render of the same tick: the stored pair still applies.
        } else {
            Count(g_counters.sameTickChanges);
            entry.quarantineUntil = serial + kQuarantineTicks;
            entry.hasPrevious = false;
        }
    } else {
        entry.hasPrevious = serial - entry.currentSerial == 1;
        if (entry.hasPrevious) {
            entry.previous = entry.current;
            entry.previousSerial = entry.currentSerial;
        }
    }
    entry.current = source;
    entry.currentSerial = serial;
    entry.hasCurrent = true;

    if (!entry.hasPrevious ||
        hands_trace::SameMatrix(entry.previous, entry.current)) {
        return;
    }
    Count(g_counters.moving);
    if (serial < entry.quarantineUntil) {
        Count(g_counters.quarantined);
        return;
    }
    if (!g_hasSample || g_sample.kind != clock::SampleKind::interpolated ||
        entry.previousSerial != g_sample.olderSerial ||
        entry.currentSerial != g_sample.newerSerial) {
        Count(g_counters.unavailable);
        return;
    }
    hands_trace::Matrix interpolated{};
    if (!InterpolateScaledRigid(entry.previous, entry.current, g_sample.alpha,
                                interpolated)) {
        Count(g_counters.rejected);
        return;
    }
    Count(g_counters.eligible);
    if (g_substitute) {
        if (g_hasLead) {
            const float dx = interpolated.values[3] - g_lead.center[0];
            const float dy = interpolated.values[7] - g_lead.center[1];
            const float dz = interpolated.values[11] - g_lead.center[2];
            if (dx * dx + dy * dy + dz * dz <=
                g_lead.radius * g_lead.radius) {
                ApplyLead(g_lead, interpolated);
                Count(g_counters.led);
            }
        }
        entry.presented = interpolated;
        entry.presentValid = true;
        entry.presentedRender.store(g_render.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
        Count(g_counters.applied);
    }
}

constexpr std::size_t kPoseTableBits = 8;
constexpr std::size_t kPoseTableSize = std::size_t{1} << kPoseTableBits;
constexpr std::size_t kPoseScanBones = 1024;

using hands_pose::DualQuaternion;
using Palette = std::array<DualQuaternion, hands_pose::kMaximumBones>;

#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) PoseEntry {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uint64_t> lastRender{0};
    std::atomic_flag busy;
    std::uint64_t currentSerial = 0;
    std::uint64_t quarantineUntil = 0;
    std::size_t boneCount = 0;
    std::size_t currentIndex = 0;
    bool hasCurrent = false;
    bool hasPrevious = false;
    bool differs = false;
    bool presentValid = false;
    std::array<Palette, 2> palettes{};
    alignas(16) Palette presented{};
};
#pragma warning(pop)

std::array<PoseEntry, kPoseTableSize> g_poseEntries{};
std::atomic<bool> g_poseSubstitute{false};

struct AtomicPoseCounters {
    std::atomic<std::uint64_t> tracked{0};
    std::atomic<std::uint64_t> changed{0};
    std::atomic<std::uint64_t> applied{0};
    std::atomic<std::uint64_t> unavailable{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> snappedBones{0};
    std::atomic<std::uint64_t> sameTickChanges{0};
    std::atomic<std::uint64_t> quarantined{0};
    std::atomic<std::uint64_t> oversize{0};
    std::atomic<std::uint64_t> tableFull{0};
    std::atomic<std::uint64_t> maximumBones{0};
    std::atomic<std::uint64_t> changedMaximumBones{0};
};

AtomicPoseCounters g_poseCounters;

void RaiseTo(std::atomic<std::uint64_t>& maximum,
             const std::uint64_t value) noexcept {
    std::uint64_t seen = maximum.load(std::memory_order_relaxed);
    while (seen < value &&
           !maximum.compare_exchange_weak(seen, value,
                                          std::memory_order_relaxed)) {
    }
}

void ClearPose(PoseEntry& entry) noexcept {
    entry.currentSerial = 0;
    entry.quarantineUntil = 0;
    entry.boneCount = 0;
    entry.currentIndex = 0;
    entry.hasCurrent = false;
    entry.hasPrevious = false;
    entry.differs = false;
    entry.presentValid = false;
}

PoseEntry* AcquirePose(const std::uintptr_t key,
                       const std::uint64_t render) noexcept {
    const std::size_t first = static_cast<std::size_t>(
        ((key >> 4) * 0x9E3779B97F4A7C15ULL) >> (64 - kPoseTableBits));
    for (std::size_t probe = 0; probe < kProbeLimit; ++probe) {
        PoseEntry& entry = g_poseEntries[(first + probe) & (kPoseTableSize - 1)];
        if (entry.key.load(std::memory_order_acquire) != key) {
            continue;
        }
        while (entry.busy.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
        if (entry.key.load(std::memory_order_relaxed) == key) {
            return &entry;
        }
        entry.busy.clear(std::memory_order_release);
    }
    for (std::size_t probe = 0; probe < kProbeLimit; ++probe) {
        PoseEntry& entry = g_poseEntries[(first + probe) & (kPoseTableSize - 1)];
        const auto reusable = [&entry, render]() noexcept {
            const std::uint64_t seen =
                entry.lastRender.load(std::memory_order_relaxed);
            return entry.key.load(std::memory_order_relaxed) == 0 ||
                   (render > seen && render - seen > kStaleRenders);
        };
        if (!reusable() ||
            entry.busy.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        if (!reusable()) {
            entry.busy.clear(std::memory_order_release);
            continue;
        }
        ClearPose(entry);
        entry.lastRender.store(0, std::memory_order_relaxed);
        entry.key.store(key, std::memory_order_release);
        return &entry;
    }
    return nullptr;
}

void ObservePose(PoseEntry& entry,
                 const DualQuaternion* const source,
                 const std::size_t boneCount) noexcept {
    const std::uint64_t serial = g_fixedSerial;
    const std::size_t bytes = boneCount * sizeof(DualQuaternion);
    Count(g_poseCounters.tracked);
    RaiseTo(g_poseCounters.maximumBones, boneCount);
    entry.presentValid = false;

    const bool sameShape = entry.hasCurrent && entry.boneCount == boneCount;
    const bool sameAsCurrent =
        sameShape && std::memcmp(entry.palettes[entry.currentIndex].data(),
                                 source, bytes) == 0;
    if (!sameShape || serial < entry.currentSerial) {
        entry.hasPrevious = false;
        entry.differs = false;
        std::memcpy(entry.palettes[entry.currentIndex].data(), source, bytes);
    } else if (serial == entry.currentSerial) {
        if (!sameAsCurrent) {
            Count(g_poseCounters.sameTickChanges);
            entry.quarantineUntil = serial + kQuarantineTicks;
            entry.hasPrevious = false;
            entry.differs = false;
            std::memcpy(entry.palettes[entry.currentIndex].data(), source,
                        bytes);
        }
    } else {
        entry.hasPrevious = serial - entry.currentSerial == 1;
        entry.differs = entry.hasPrevious && !sameAsCurrent;
        if (!sameAsCurrent) {
            // The slot that held the older pose becomes the newer one.
            entry.currentIndex ^= 1U;
            std::memcpy(entry.palettes[entry.currentIndex].data(), source,
                        bytes);
        }
    }
    entry.boneCount = boneCount;
    entry.currentSerial = serial;
    entry.hasCurrent = true;

    if (!entry.hasPrevious || !entry.differs) {
        return;
    }
    Count(g_poseCounters.changed);
    RaiseTo(g_poseCounters.changedMaximumBones, boneCount);
    if (serial < entry.quarantineUntil) {
        Count(g_poseCounters.quarantined);
        return;
    }
    if (!g_hasSample || g_sample.kind != clock::SampleKind::interpolated ||
        g_sample.newerSerial != entry.currentSerial ||
        g_sample.olderSerial + 1 != g_sample.newerSerial) {
        Count(g_poseCounters.unavailable);
        return;
    }
    const DualQuaternion* const newer =
        entry.palettes[entry.currentIndex].data();
    const DualQuaternion* const older =
        entry.palettes[entry.currentIndex ^ 1U].data();
    std::size_t snappedBones = 0;
    if (!hands_pose::BlendPalettePerBone(older, newer, boneCount,
                                         g_sample.alpha,
                                         entry.presented.data(),
                                         snappedBones)) {
        Count(g_poseCounters.rejected);
        return;
    }
    g_poseCounters.snappedBones.fetch_add(snappedBones,
                                          std::memory_order_relaxed);
    if (g_poseSubstitute.load(std::memory_order_relaxed)) {
        entry.presentValid = true;
        Count(g_poseCounters.applied);
    }
}

} // namespace

void ConfigurePose(const bool substitute) noexcept {
    g_poseSubstitute.store(substitute, std::memory_order_relaxed);
}

const void* PresentPalette(const void* const mesh,
                           const void* const palette,
                           const std::size_t boneCount) noexcept {
    if (mesh == nullptr || palette == nullptr || boneCount == 0 ||
        boneCount > kPoseScanBones ||
        !g_active.load(std::memory_order_acquire)) {
        return palette;
    }
    if (boneCount > hands_pose::kMaximumBones) {
        Count(g_poseCounters.oversize);
        RaiseTo(g_poseCounters.maximumBones, boneCount);
        return palette;
    }
    const std::uint64_t render = g_render.load(std::memory_order_relaxed);
    PoseEntry* const entry =
        AcquirePose(reinterpret_cast<std::uintptr_t>(mesh), render);
    if (entry == nullptr) {
        Count(g_poseCounters.tableFull);
        return palette;
    }
    if (entry->lastRender.load(std::memory_order_relaxed) != render) {
        entry->lastRender.store(render, std::memory_order_relaxed);
        ObservePose(*entry, static_cast<const DualQuaternion*>(palette),
                    boneCount);
    }
    const void* const result =
        entry->presentValid && entry->boneCount == boneCount
            ? static_cast<const void*>(entry->presented.data())
            : palette;
    entry->busy.clear(std::memory_order_release);
    return result;
}

PoseCounters TakePoseCounters() noexcept {
    const auto take = [](std::atomic<std::uint64_t>& counter) noexcept {
        return counter.exchange(0, std::memory_order_acq_rel);
    };
    PoseCounters result{};
    result.tracked = take(g_poseCounters.tracked);
    result.changed = take(g_poseCounters.changed);
    result.applied = take(g_poseCounters.applied);
    result.unavailable = take(g_poseCounters.unavailable);
    result.rejected = take(g_poseCounters.rejected);
    result.snappedBones = take(g_poseCounters.snappedBones);
    result.sameTickChanges = take(g_poseCounters.sameTickChanges);
    result.quarantined = take(g_poseCounters.quarantined);
    result.oversize = take(g_poseCounters.oversize);
    result.tableFull = take(g_poseCounters.tableFull);
    result.maximumBones = take(g_poseCounters.maximumBones);
    result.changedMaximumBones = take(g_poseCounters.changedMaximumBones);
    return result;
}

bool InterpolateScaledRigid(const hands_trace::Matrix& older,
                            const hands_trace::Matrix& newer,
                            const double alpha,
                            hands_trace::Matrix& result) noexcept {
    result = {};
    if (!hands_trace::IsFiniteAffine(older) ||
        !hands_trace::IsFiniteAffine(newer)) {
        return false;
    }
    hands_trace::Matrix olderRigid = older;
    hands_trace::Matrix newerRigid = newer;
    std::array<float, 3> scale{};
    for (std::size_t column = 0; column < 3; ++column) {
        const float olderScale = ColumnLength(older, column);
        const float newerScale = ColumnLength(newer, column);
        if (!(olderScale > 1.0e-4F) || !(newerScale > 1.0e-4F) ||
            std::abs(olderScale - newerScale) >
                1.0e-3F * std::max(olderScale, newerScale)) {
            return false;
        }
        ScaleColumn(olderRigid, column, 1.0F / olderScale);
        ScaleColumn(newerRigid, column, 1.0F / newerScale);
        scale[column] = newerScale;
    }
    if (!hands_trace::InterpolateRigid(olderRigid, newerRigid, alpha,
                                       result)) {
        result = {};
        return false;
    }
    for (std::size_t column = 0; column < 3; ++column) {
        ScaleColumn(result, column, scale[column]);
    }
    return hands_trace::IsFiniteAffine(result);
}

void ApplyLead(const LeadTransform& lead,
               hands_trace::Matrix& matrix) noexcept {
    const float* const r = lead.rotation;
    hands_trace::Matrix result = matrix;
    for (std::size_t column = 0; column < 3; ++column) {
        const float x = matrix.values[column];
        const float y = matrix.values[4 + column];
        const float z = matrix.values[8 + column];
        result.values[column] = r[0] * x + r[1] * y + r[2] * z;
        result.values[4 + column] = r[3] * x + r[4] * y + r[5] * z;
        result.values[8 + column] = r[6] * x + r[7] * y + r[8] * z;
    }
    const float x = matrix.values[3] - lead.center[0];
    const float y = matrix.values[7] - lead.center[1];
    const float z = matrix.values[11] - lead.center[2];
    result.values[3] = r[0] * x + r[1] * y + r[2] * z + lead.center[0];
    result.values[7] = r[3] * x + r[4] * y + r[5] * z + lead.center[1];
    result.values[11] = r[6] * x + r[7] * y + r[8] * z + lead.center[2];
    matrix = result;
}

void BeginRender(const std::uint64_t fixedSerial,
                 const clock::Sample* const sample,
                 const bool substitute,
                 const LeadTransform* const lead) noexcept {
    g_hasLead = lead != nullptr;
    g_lead = lead != nullptr ? *lead : LeadTransform{};
    g_fixedSerial = fixedSerial;
    g_hasSample = sample != nullptr;
    g_sample = sample != nullptr ? *sample : clock::Sample{};
    g_substitute = substitute;
    g_render.fetch_add(1, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

void EndRender() noexcept {
    g_active.store(false, std::memory_order_release);
}

const hands_trace::Matrix* Present(
    const void* const subMesh,
    const hands_trace::Matrix* const original) noexcept {
    if (subMesh == nullptr || original == nullptr ||
        !g_active.load(std::memory_order_acquire)) {
        return original;
    }
    const std::uint64_t render = g_render.load(std::memory_order_relaxed);
    Entry* const entry =
        Acquire(reinterpret_cast<std::uintptr_t>(subMesh), render);
    if (entry == nullptr) {
        Count(g_counters.tableFull);
        return original;
    }
    if (entry->lastRender.load(std::memory_order_relaxed) != render) {
        entry->lastRender.store(render, std::memory_order_relaxed);
        Observe(*entry, *original);
    } else if (entry->hasCurrent &&
               !hands_trace::SameMatrix(*original, entry->current)) {
        // The engine returned a different matrix for the same submesh within
        // one render (for example a per-view transform). One stored
        // interpolant cannot represent that, so stop substituting it.
        Count(g_counters.frameVariants);
        entry->quarantineUntil = g_fixedSerial + kQuarantineTicks;
        entry->hasPrevious = false;
        entry->presentValid = false;
    }
    const hands_trace::Matrix* const result =
        entry->presentValid ? &entry->presented : original;
    Unlock(*entry);
    return result;
}

bool WasPresentedRecently(const void* const renderable) noexcept {
    if (renderable == nullptr || !g_active.load(std::memory_order_acquire)) {
        return false;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(renderable);
    const std::uint64_t render = g_render.load(std::memory_order_relaxed);
    const std::size_t first = SlotFor(key);
    for (std::size_t probe = 0; probe < kProbeLimit; ++probe) {
        const Entry& entry = g_entries[(first + probe) & (kTableSize - 1)];
        const std::uintptr_t candidate =
            entry.key.load(std::memory_order_acquire);
        if (candidate == 0) {
            return false;
        }
        if (candidate != key) {
            continue;
        }
        // The question may be asked before this render's matrix call, so a
        // slot presented in the previous render still qualifies.
        const std::uint64_t presented =
            entry.presentedRender.load(std::memory_order_relaxed);
        return presented != 0 && render >= presented &&
               render - presented <= 1;
    }
    return false;
}

int RefreshUpdateCount(const void* const renderable,
                       const int original) noexcept {
    if (!WasPresentedRecently(renderable)) {
        return original;
    }
    Count(g_counters.updateCountRefreshes);
    return static_cast<int>(
        static_cast<std::uint32_t>(original) +
        static_cast<std::uint32_t>(g_render.load(std::memory_order_relaxed)));
}

void MarkPresented(const void* const renderable) noexcept {
    if (renderable == nullptr || !g_active.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t render = g_render.load(std::memory_order_relaxed);
    Entry* const entry =
        Acquire(reinterpret_cast<std::uintptr_t>(renderable), render);
    if (entry == nullptr) {
        Count(g_counters.tableFull);
        return;
    }
    entry->lastRender.store(render, std::memory_order_relaxed);
    entry->presentedRender.store(render, std::memory_order_relaxed);
    Unlock(*entry);
}

Counters TakeCounters() noexcept {
    const auto take = [](std::atomic<std::uint64_t>& counter) noexcept {
        return counter.exchange(0, std::memory_order_acq_rel);
    };
    Counters result{};
    result.tracked = take(g_counters.tracked);
    result.moving = take(g_counters.moving);
    result.eligible = take(g_counters.eligible);
    result.applied = take(g_counters.applied);
    result.unavailable = take(g_counters.unavailable);
    result.rejected = take(g_counters.rejected);
    result.invalid = take(g_counters.invalid);
    result.sameTickChanges = take(g_counters.sameTickChanges);
    result.frameVariants = take(g_counters.frameVariants);
    result.quarantined = take(g_counters.quarantined);
    result.tableFull = take(g_counters.tableFull);
    result.evictions = take(g_counters.evictions);
    result.updateCountRefreshes = take(g_counters.updateCountRefreshes);
    result.led = take(g_counters.led);
    return result;
}

void Reset() noexcept {
    g_active.store(false, std::memory_order_release);
    for (Entry& entry : g_entries) {
        Lock(entry);
        Clear(entry);
        entry.lastRender.store(0, std::memory_order_relaxed);
        entry.presentedRender.store(0, std::memory_order_relaxed);
        entry.key.store(0, std::memory_order_release);
        Unlock(entry);
    }
    for (PoseEntry& entry : g_poseEntries) {
        while (entry.busy.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
        ClearPose(entry);
        entry.lastRender.store(0, std::memory_order_relaxed);
        entry.key.store(0, std::memory_order_release);
        entry.busy.clear(std::memory_order_release);
    }
    g_render.store(0, std::memory_order_relaxed);
    (void)TakeCounters();
    (void)TakePoseCounters();
}

} // namespace atb::world
