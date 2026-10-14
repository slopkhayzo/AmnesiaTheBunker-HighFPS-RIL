#include "particle_runtime.h"

#include "guarded_memory.h"

#include <Windows.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace atb::particle_runtime {
namespace {

// iParticleEmitter layout, GOG build 57301760292462330.
constexpr std::size_t kParticleList = 0x1E8;  // cParticle** of live particles
constexpr std::size_t kParticleCount = 0x200; // unsigned
constexpr std::size_t kParticleMaximum = 0x204;
// cParticle: position, then the position before the last motion update.
constexpr std::size_t kParticleEndpoints = 6 * sizeof(float);

constexpr std::uint32_t kMaximumParticles = 2048;
constexpr std::uint32_t kMaximumCapacity = 1U << 16;
// A particle that moved further than this in one tick was respawned or
// teleported, not flown.
constexpr float kMaximumStepSquared = 5.0F * 5.0F;
constexpr std::size_t kTableSize = 1024; // power of two

using UpdateGraphicsForViewport = std::uint64_t (*)(void* emitter,
                                                    void* frustum);
UpdateGraphicsForViewport g_original = nullptr;

// Set on the render thread around the scene render, read by whichever
// thread runs an emitter's graphics update during it.
std::atomic<bool> g_interpolating{false};
std::atomic<std::uint64_t> g_newerSerial{0};
std::atomic<double> g_alpha{0.0};
std::atomic<DWORD> g_renderThread{0};

// Which emitters were simulated in the tick being presented. An emitter the
// game did not update keeps its old pair of positions, and replaying that
// move every tick would make its particles jump back and forth.
struct Entry {
    const void* emitter = nullptr;
    std::uint64_t fingerprint = 0;
    std::uint64_t changedSerial = 0;
    bool changedOnce = false;
    bool busy = false;
};
std::array<Entry, kTableSize> g_table{};
std::size_t g_tableUsed = 0;
std::atomic_flag g_tableLock = ATOMIC_FLAG_INIT;

std::atomic<std::uint64_t> g_updates{0};
std::atomic<std::uint64_t> g_offMainThread{0};
std::atomic<std::uint64_t> g_interpolated{0};
std::atomic<std::uint64_t> g_particles{0};
std::atomic<std::uint64_t> g_idle{0};
std::atomic<std::uint64_t> g_unavailable{0};
std::atomic<std::uint64_t> g_oversize{0};
std::atomic<std::uint64_t> g_invalid{0};
std::atomic<std::uint64_t> g_busy{0};
std::atomic<std::uint64_t> g_tableResets{0};
std::atomic<std::uint32_t> g_largestEmitter{0};

struct TableLock {
    TableLock() noexcept {
        while (g_tableLock.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
    }
    ~TableLock() { g_tableLock.clear(std::memory_order_release); }
    TableLock(const TableLock&) = delete;
    TableLock& operator=(const TableLock&) = delete;
};

std::size_t Bucket(const void* const emitter) noexcept {
    const std::uint64_t value =
        reinterpret_cast<std::uintptr_t>(emitter) * 0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>(value >> 40) & (kTableSize - 1);
}

// Must hold the lock. Null when the table is full of emitters being drawn.
Entry* Find(const void* const emitter) noexcept {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::size_t index = Bucket(emitter);
        for (std::size_t probe = 0; probe < kTableSize; ++probe) {
            Entry& entry = g_table[index];
            if (entry.emitter == emitter) {
                return &entry;
            }
            if (entry.emitter == nullptr) {
                if (g_tableUsed >= kTableSize / 2) {
                    break; // keep probes short: start over below
                }
                entry = {};
                entry.emitter = emitter;
                ++g_tableUsed;
                return &entry;
            }
            index = (index + 1) & (kTableSize - 1);
        }
        // Emitters come and go with the map; forget them all rather than
        // track which ones died. Nothing is lost but one tick of smoothing.
        for (const Entry& entry : g_table) {
            if (entry.busy) {
                return nullptr;
            }
        }
        g_table.fill({});
        g_tableUsed = 0;
        g_tableResets.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

void Release(const void* const emitter) noexcept {
    const TableLock lock;
    std::size_t index = Bucket(emitter);
    for (std::size_t probe = 0; probe < kTableSize; ++probe) {
        Entry& entry = g_table[index];
        if (entry.emitter == emitter) {
            entry.busy = false;
            return;
        }
        if (entry.emitter == nullptr) {
            return;
        }
        index = (index + 1) & (kTableSize - 1);
    }
}

std::uint64_t Mix(std::uint64_t hash, const void* const bytes,
                  const std::size_t size) noexcept {
    const auto* const data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ data[index]) * 0x100000001B3ULL;
    }
    return hash;
}

struct Saved {
    float* particle;
    float endpoints[6];
};

std::uint64_t HookedUpdateGraphicsForViewport(void* const emitter,
                                              void* const frustum) {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    if (GetCurrentThreadId() != g_renderThread.load(std::memory_order_relaxed)) {
        g_offMainThread.fetch_add(1, std::memory_order_relaxed);
    }
    if (emitter == nullptr || !g_interpolating.load(std::memory_order_acquire)) {
        g_unavailable.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }
    const std::uint64_t serial = g_newerSerial.load(std::memory_order_relaxed);
    const float back =
        static_cast<float>(g_alpha.load(std::memory_order_relaxed) - 1.0);

    const auto* const base = static_cast<const std::uint8_t*>(emitter);
    float** list = nullptr;
    std::uint32_t count = 0;
    std::uint32_t maximum = 0;
    if (!memory::Read(base + kParticleList, list) ||
        !memory::Read(base + kParticleCount, count) ||
        !memory::Read(base + kParticleMaximum, maximum) ||
        count > maximum || maximum > kMaximumCapacity ||
        (count != 0 &&
         !memory::IsPlausibleRange(list, count * sizeof(float*)))) {
        g_invalid.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }
    if (count == 0 || !(back < 0.0F) || back < -1.0F) {
        return g_original(emitter, frustum); // nothing to move
    }
    std::uint32_t largest = g_largestEmitter.load(std::memory_order_relaxed);
    while (largest < count &&
           !g_largestEmitter.compare_exchange_weak(largest, count,
                                                   std::memory_order_relaxed)) {
    }
    if (count > kMaximumParticles) {
        g_oversize.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }

    // Was this emitter simulated in the tick now being presented? Its first
    // and last particles say so without walking the whole list.
    std::uint64_t fingerprint = 0xCBF29CE484222325ULL ^ count;
    const bool readable = memory::Guarded([&]() noexcept {
        fingerprint = Mix(fingerprint, list[0], kParticleEndpoints);
        fingerprint = Mix(fingerprint, list[count - 1], kParticleEndpoints);
    });
    if (!readable) {
        g_invalid.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }
    bool fresh = false;
    bool taken = false;
    {
        const TableLock lock;
        Entry* const entry = Find(emitter);
        if (entry != nullptr && !entry->busy) {
            if (entry->fingerprint != fingerprint) {
                // The first sight of an emitter proves nothing about when
                // it last moved.
                if (entry->changedOnce) {
                    entry->changedSerial = serial;
                }
                entry->changedOnce = true;
                entry->fingerprint = fingerprint;
            }
            fresh = entry->changedSerial == serial && serial != 0;
            if (fresh) {
                entry->busy = true;
            }
            taken = true;
        }
    }
    if (!taken) {
        g_busy.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }
    if (!fresh) {
        g_idle.fetch_add(1, std::memory_order_relaxed);
        return g_original(emitter, frustum);
    }

    // Shift every particle back along its last move. The previous position
    // is shifted with it, so anything drawn from the pair (stretched
    // particles) keeps its length and direction.
    std::array<Saved, kMaximumParticles> saved;
    std::uint32_t savedCount = 0;
    const bool staged = memory::Guarded([&]() noexcept {
        for (std::uint32_t index = 0; index < count; ++index) {
            float* const particle = list[index];
            if (!memory::IsPlausibleRange(particle, kParticleEndpoints)) {
                continue;
            }
            const float moveX = particle[0] - particle[3];
            const float moveY = particle[1] - particle[4];
            const float moveZ = particle[2] - particle[5];
            const float lengthSquared =
                moveX * moveX + moveY * moveY + moveZ * moveZ;
            if (!(lengthSquared > 0.0F) ||
                !(lengthSquared <= kMaximumStepSquared)) {
                continue; // at rest, respawned, or not finite
            }
            Saved& slot = saved[savedCount];
            slot.particle = particle;
            std::memcpy(slot.endpoints, particle, kParticleEndpoints);
            ++savedCount;
            particle[0] += back * moveX;
            particle[1] += back * moveY;
            particle[2] += back * moveZ;
            particle[3] += back * moveX;
            particle[4] += back * moveY;
            particle[5] += back * moveZ;
        }
    });

    const std::uint64_t result = g_original(emitter, frustum);

    // Put back exactly what the simulation left, whatever happened above.
    const bool restored = memory::Guarded([&]() noexcept {
        for (std::uint32_t index = 0; index < savedCount; ++index) {
            std::memcpy(saved[index].particle, saved[index].endpoints,
                        kParticleEndpoints);
        }
    });
    Release(emitter);
    if (!staged || !restored) {
        g_invalid.fetch_add(1, std::memory_order_relaxed);
    }
    g_interpolated.fetch_add(1, std::memory_order_relaxed);
    g_particles.fetch_add(savedCount, std::memory_order_relaxed);
    return result;
}

} // namespace

bool Install(const std::uintptr_t updateGraphicsForViewport,
             std::wstring& error) noexcept {
    error.clear();
    if (updateGraphicsForViewport == 0) {
        error = L"Particle mapping is missing";
        return false;
    }
    void* const target = reinterpret_cast<void*>(updateGraphicsForViewport);
    MH_STATUS status = MH_CreateHook(
        target, reinterpret_cast<void*>(&HookedUpdateGraphicsForViewport),
        reinterpret_cast<void**>(&g_original));
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            (void)MH_RemoveHook(target);
        }
    }
    if (status != MH_OK) {
        g_original = nullptr;
        error = L"Could not hook the particle graphics update (MinHook "
                L"status " +
                std::to_wstring(status) + L")";
        return false;
    }
    return true;
}

void BeginRender(const bool interpolating,
                 const std::uint64_t newerSerial,
                 const double alpha) noexcept {
    g_renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_newerSerial.store(newerSerial, std::memory_order_relaxed);
    g_alpha.store(alpha, std::memory_order_relaxed);
    g_interpolating.store(interpolating && std::isfinite(alpha) &&
                              alpha >= 0.0 && alpha <= 1.0,
                          std::memory_order_release);
}

void EndRender() noexcept {
    g_interpolating.store(false, std::memory_order_release);
}

Counters TakeCounters() noexcept {
    Counters counters{};
    counters.updates = g_updates.exchange(0, std::memory_order_acq_rel);
    counters.offMainThread =
        g_offMainThread.exchange(0, std::memory_order_acq_rel);
    counters.interpolated =
        g_interpolated.exchange(0, std::memory_order_acq_rel);
    counters.particles = g_particles.exchange(0, std::memory_order_acq_rel);
    counters.idle = g_idle.exchange(0, std::memory_order_acq_rel);
    counters.unavailable = g_unavailable.exchange(0, std::memory_order_acq_rel);
    counters.oversize = g_oversize.exchange(0, std::memory_order_acq_rel);
    counters.invalid = g_invalid.exchange(0, std::memory_order_acq_rel);
    counters.busy = g_busy.exchange(0, std::memory_order_acq_rel);
    counters.tableResets = g_tableResets.exchange(0, std::memory_order_acq_rel);
    counters.largestEmitter =
        g_largestEmitter.exchange(0, std::memory_order_acq_rel);
    return counters;
}

} // namespace atb::particle_runtime
