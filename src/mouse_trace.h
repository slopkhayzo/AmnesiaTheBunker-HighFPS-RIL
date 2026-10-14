#pragma once

#include <cstdint>
#include <string>

// The game's mouse path, observed without consuming, reordering, or injecting
// input: relative motion is seen through an SDL event watch and the game's
// own SDL_GetRelativeMouseState calls are counted on the way back to it.
//
// On top of that measurement sits the presentation-rate look preview. The
// game reads the mouse once per fixed tick, so an interpolated camera shows
// mouse input up to a tick late. The preview reports how far the view should
// lead the interpolated camera: the part of the latest tick's mouse turn the
// interpolation has not shown yet, plus the counts that have arrived since
// and that the game will consume on its next tick. Everything it needs is
// learned from the game's own response and re-checked every tick.
namespace atb::mouse {

struct Report {
    std::uint64_t motionEvents = 0;   // SDL_MOUSEMOTION events observed
    std::uint64_t motionFrames = 0;   // presented frames in which any arrived
    std::uint64_t consumeCalls = 0;   // game calls to GetRelativeMouseState
    std::uint64_t consumeNonZero = 0; // of those, calls that returned motion
    std::int64_t observedX = 0;       // counts seen by the watch
    std::int64_t observedY = 0;
    std::int64_t consumedX = 0;       // counts handed to the game
    std::int64_t consumedY = 0;
    std::int64_t maximumDrift = 0;    // largest |observed - consumed| after a
                                      // consume, over the whole session
    std::uint64_t ticks = 0;          // camera ticks analysed
    std::uint64_t ticksWithMotion = 0;
    std::uint64_t turnsWithoutMotion = 0; // camera turned, no counts consumed

    // Look preview.
    bool scaleReady = false;      // radians-per-count learned for both axes
    bool axesReady = false;       // view-axis convention and signs learned
    // Learning progress per candidate layout (rows, then columns): votes for
    // +yaw, -yaw, +pitch, -pitch, and ticks no sign choice explained.
    int axisVotes[2][5]{};
    double yawPerCount = 0.0;
    double pitchPerCount = 0.0;
    double pitchLow = 0.0;        // learned pitch limits
    double pitchHigh = 0.0;
    std::uint64_t leadFrames = 0;       // frames a lead was offered
    std::uint64_t mismatchTicks = 0;    // ticks the camera turned by neither
                                        // nothing nor scale * counts
    std::uint64_t suspendedTicks = 0;   // ticks with counts on a detached axis
    std::uint64_t clampedTicks = 0;     // ticks the game limited the pitch
    std::uint64_t scaleChanges = 0;     // times a new scale was adopted
    std::uint64_t detaches = 0;         // times an attached axis was detached

    // The first few ticks of the interval that detached an axis, for
    // diagnosis.
    struct Miss {
        wchar_t axis = L'?';
        std::int64_t counts = 0;
        double angle = 0.0;    // what the camera did, radians
        double expected = 0.0; // scale * counts
    };
    static constexpr std::size_t kMissCapacity = 6;
    Miss misses[kMissCapacity]{};
    std::size_t missCount = 0;
};

struct Lead {
    bool active = false;
    float yaw = 0.0F;   // radians to add to the interpolated camera
    float pitch = 0.0F;
    float pitchLow = 0.0F;
    float pitchHigh = 0.0F;
};

// Hooks SDL_GetRelativeMouseState in the already loaded SDL2.dll. MinHook must
// be initialized. The event watch is added on the game's first consume call.
[[nodiscard]] bool InstallTrace(std::wstring& error) noexcept;
void ConfigurePreview(bool enabled) noexcept;

// Main thread, once per presented frame.
void OnPresent() noexcept;
// Main thread, once per completed camera tick. `view` is the engine's
// authoritative 4x4 view matrix for this pose, or null if unavailable.
void OnCameraTick(std::uint64_t serial,
                  float yaw,
                  float pitch,
                  const float* view,
                  bool valid) noexcept;

// Main thread, once per scene render. Pumps SDL so motion that arrived since
// the game's last tick is visible, then reports the lead for a camera
// interpolated toward tick `newerSerial` at `alpha`.
[[nodiscard]] Lead ComputeLead(std::uint64_t newerSerial,
                               double alpha) noexcept;
// World-space rotation (row-major 3x3) that turns something rigidly attached
// to the interpolated camera into the led camera's frame.
[[nodiscard]] bool BuildLeadRotation(float yaw,
                                     float pitch,
                                     float rotation[9]) noexcept;

// The operating-system cursor as SDL sees it, and the motion counted so far.
struct CursorState {
    bool available = false;
    bool relativeMode = false; // SDL reports motion only; x and y stand still
    int x = 0;                 // window pixels
    int y = 0;
    std::int64_t observedX = 0; // motion counts seen by the event watch
    std::int64_t observedY = 0;
};

// Main thread. Moves pending OS messages into SDL's queue, at most once per
// presented frame; nothing is taken out of the queue.
void PumpForFrame() noexcept;
[[nodiscard]] CursorState ReadCursor() noexcept;

[[nodiscard]] Report TakeReport() noexcept;

} // namespace atb::mouse
