#pragma once

#include <cstdint>
#include <string>

// Particle interpolation.
//
// A particle emitter moves its particles once per simulation tick and keeps,
// for every particle, the position before that move next to the new one.
// The emitter's per-view graphics update turns the particle list into
// vertices every frame. Around that update this layer shifts each particle
// (its position and its remembered previous position alike) back along the
// last tick's move by the part of the tick not yet presented, and puts the
// stored values back afterwards. Nothing is simulated a second time: spawn,
// death, collision, colour and size stay exactly as the tick left them.
namespace atb::particle_runtime {

struct Counters {
    std::uint64_t updates = 0;       // graphics updates seen
    std::uint64_t offMainThread = 0; // of those, not on the render thread
    std::uint64_t interpolated = 0;  // updates drawn from shifted particles
    std::uint64_t particles = 0;     // particles shifted
    std::uint64_t idle = 0;          // emitter not simulated this tick
    std::uint64_t unavailable = 0;   // no interpolation clock for the frame
    std::uint64_t oversize = 0;      // more particles than the layer handles
    std::uint64_t invalid = 0;       // layout or memory not as expected
    std::uint64_t busy = 0;          // emitter already being drawn elsewhere
    std::uint64_t tableResets = 0;
    std::uint32_t largestEmitter = 0; // particles
};

// Hooks the emitter's per-view graphics update. MinHook must be initialized.
[[nodiscard]] bool Install(std::uintptr_t updateGraphicsForViewport,
                           std::wstring& error) noexcept;

// Render thread, around the scene render. `interpolating` is false when the
// frame shows the newest tick as it is.
void BeginRender(bool interpolating,
                 std::uint64_t newerSerial,
                 double alpha) noexcept;
void EndRender() noexcept;

[[nodiscard]] Counters TakeCounters() noexcept;

} // namespace atb::particle_runtime
