#pragma once

#include <cstdint>
#include <string>

// Per-frame record of where each presented frame's time went and which view
// it showed, for measuring pacing. The main thread fills one fixed-size
// record per frame into a preallocated ring; a background thread writes the
// ring to `<plugin>.frames.bin`. Nothing is allocated or written on the main
// thread after Start.
namespace atb::frame_trace {

enum class Stamp : std::uint32_t {
    fixedBegin,     // first fixed update accepted this frame
    fixedEnd,       // last fixed update completed
    variableEnd,    // variable update completed
    renderBegin,    // scene render hook entered
    paceWaitEnd,    // the even pacer's wait before the render is over
    renderOriginal, // the plugin's staging is done; the engine's render starts
    renderEnd,      // scene render hook left
    swapEnter,      // swap hook entered
    waitEnd,        // the limiter's wait is over
    count,
};

#pragma pack(push, 4)
struct Record {
    std::int64_t presentedQpc = 0; // the swap returned
    // QPC ticks since the previous frame's presentedQpc; zero if not reached.
    std::uint32_t stamps[static_cast<std::size_t>(Stamp::count)]{};
    float viewYaw = 0.0F; // what was staged for the render, radians
    float viewPitch = 0.0F;
    float leadYaw = 0.0F; // the mouse preview's part of it
    float leadPitch = 0.0F;
    float alpha = 0.0F;
    std::uint32_t serial = 0; // newer tick of the interpolated pair
    std::uint16_t fixedUpdates = 0;
    // Bit 0: a view was staged; bit 1: lead applied; bit 2: the scene has a
    // listener camera (gameplay, not a menu); bits 8-15: frames per tick
    // chosen by the even pacer, zero when it is not pacing.
    std::uint16_t flags = 0;
};
#pragma pack(pop)

struct FileHeader {
    char magic[8] = {'A', 'T', 'B', 'F', 'R', 'M', '0', '2'};
    std::int64_t qpcFrequency = 0;
    std::uint32_t recordSize = sizeof(Record);
    std::uint32_t stampCount = static_cast<std::uint32_t>(Stamp::count);
};

// Creates the file next to the plugin and starts the writer thread.
[[nodiscard]] bool Start(std::int64_t qpcFrequency,
                         std::wstring& error) noexcept;

// Main thread only. All are no-ops until Start has succeeded.
void Mark(Stamp stamp) noexcept;
void CountFixedUpdate() noexcept;
void SetView(float viewYaw,
             float viewPitch,
             float leadYaw,
             float leadPitch,
             bool leadApplied,
             float alpha,
             std::uint64_t serial) noexcept;
void MarkGameplay() noexcept;
void SetFramesPerTick(unsigned int framesPerTick) noexcept;
void EndFrame(std::int64_t presentedQpc) noexcept;

// Records lost because the writer fell behind, since the last call.
[[nodiscard]] std::uint64_t TakeDropped() noexcept;

} // namespace atb::frame_trace
