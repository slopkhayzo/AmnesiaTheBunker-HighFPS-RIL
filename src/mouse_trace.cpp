#include "mouse_trace.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace atb::mouse {
namespace {

constexpr std::uint32_t kSdlMouseMotion = 0x400;
constexpr std::uint32_t kSdlMouseButtonDown = 0x401;
constexpr std::size_t kMotionXRelOffset = 28;
constexpr std::size_t kMotionYRelOffset = 32;
constexpr double kPi = 3.14159265358979323846;

using SdlGetRelativeMouseState = std::uint32_t (*)(int* x, int* y);
using SdlEventFilter = int (*)(void* userdata, void* event);
using SdlAddEventWatch = void (*)(SdlEventFilter filter, void* userdata);
using SdlPumpEvents = void (*)();
using SdlGetMouseState = std::uint32_t (*)(int* x, int* y);
using SdlGetRelativeMouseMode = int (*)();

SdlGetRelativeMouseState g_originalGetRelativeMouseState = nullptr;
SdlAddEventWatch g_addEventWatch = nullptr;
SdlPumpEvents g_pumpEvents = nullptr;
SdlGetMouseState g_getMouseState = nullptr;
SdlGetRelativeMouseMode g_getRelativeMouseMode = nullptr;
std::uint64_t g_pumpedFrame = ~std::uint64_t{0};
std::atomic<bool> g_watchAdded{false};
bool g_previewEnabled = false;

std::atomic<std::uint64_t> g_frame{0};
std::atomic<std::uint64_t> g_lastMotionFrame{~std::uint64_t{0}};
std::atomic<std::uint64_t> g_motionEvents{0};
std::atomic<std::uint64_t> g_motionFrames{0};
std::atomic<std::uint64_t> g_consumeCalls{0};
std::atomic<std::uint64_t> g_consumeNonZero{0};
std::atomic<std::int64_t> g_observedX{0};
std::atomic<std::int64_t> g_observedY{0};
std::atomic<std::int64_t> g_consumedX{0};
std::atomic<std::int64_t> g_consumedY{0};
std::atomic<std::int64_t> g_maximumDrift{0};
// Mouse button presses, and the observed totals at the latest one.
std::atomic<std::uint32_t> g_pressCount{0};
std::atomic<std::int64_t> g_pressObservedX{0};
std::atomic<std::int64_t> g_pressObservedY{0};

// Everything below belongs to the main thread.

// Radians per count, learned from ticks where the camera's change is an exact
// multiple of the counts the game consumed. A different value has to repeat
// before it replaces the current one, so one odd tick changes nothing.
struct ScaleEstimator {
    static constexpr int kRequiredAgreement = 12;
    static constexpr std::int64_t kMinimumCounts = 8;

    double value = 0.0;
    double candidate = 0.0;
    int agreement = 0;
    bool ready = false;

    // Returns true when a new value was adopted.
    bool Observe(const std::int64_t counts, const double angle) noexcept {
        if (std::llabs(counts) < kMinimumCounts) {
            return false;
        }
        const double ratio = angle / static_cast<double>(counts);
        if (!std::isfinite(ratio) || ratio == 0.0) {
            agreement = 0;
            return false;
        }
        if (agreement != 0 &&
            std::abs(ratio - candidate) <= 2.0e-3 * std::abs(candidate)) {
            ++agreement;
        } else {
            candidate = ratio;
            agreement = 1;
        }
        if (agreement < kRequiredAgreement) {
            return false;
        }
        const bool changed =
            !ready || std::abs(candidate - value) > 2.0e-3 * std::abs(value);
        value = candidate;
        ready = true;
        return changed;
    }
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

double Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

Vec3 RotateAboutY(const Vec3& v, const double angle) noexcept {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

Vec3 RotateAboutAxis(const Vec3& v,
                     const Vec3& axis,
                     const double angle) noexcept {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const Vec3 cross = Cross(axis, v);
    const double along = Dot(axis, v) * (1.0 - c);
    return {v.x * c + cross.x * s + axis.x * along,
            v.y * c + cross.y * s + axis.y * along,
            v.z * c + cross.z * s + axis.z * along};
}

// The view matrix holds the camera's right and up axes either as rows or as
// columns, and the yaw and pitch fields may turn either way about them. None
// of that is assumed. For every tick the camera turned, each layout predicts
// the new right and up axes from the old ones under all four sign choices;
// the best choice gets the vote, and a tick no choice explains counts against
// the layout. Ordinary diagonal mouse movement is enough: for the correct
// layout the right axis depends on yaw alone, so mixed ticks also tell the
// two layouts apart.
struct AxisLayout {
    static constexpr int kRequiredVotes = 8;

    bool columns = false;
    int yawPositive = 0;
    int yawNegative = 0;
    int pitchPositive = 0;
    int pitchNegative = 0;
    int unexplained = 0;

    [[nodiscard]] Vec3 Right(const float* view) const noexcept {
        return columns ? Vec3{view[0], view[4], view[8]}
                       : Vec3{view[0], view[1], view[2]};
    }
    [[nodiscard]] Vec3 Up(const float* view) const noexcept {
        return columns ? Vec3{view[1], view[5], view[9]}
                       : Vec3{view[4], view[5], view[6]};
    }
    [[nodiscard]] bool Ready() const noexcept {
        const int yawVotes = std::max(yawPositive, yawNegative);
        const int pitchVotes = std::max(pitchPositive, pitchNegative);
        return yawVotes >= kRequiredVotes && pitchVotes >= kRequiredVotes &&
               std::min(yawPositive, yawNegative) == 0 &&
               std::min(pitchPositive, pitchNegative) == 0 &&
               unexplained * 8 <= yawVotes + pitchVotes;
    }
    [[nodiscard]] double YawSign() const noexcept {
        return yawPositive > yawNegative ? 1.0 : -1.0;
    }
    [[nodiscard]] double PitchSign() const noexcept {
        return pitchPositive > pitchNegative ? 1.0 : -1.0;
    }
};

constexpr double kAgreement = 0.99995;
constexpr double kMinimumTurn = 2.0e-3;
constexpr double kMaximumTurn = 0.5;
constexpr double kMaximumLead = 0.6;

ScaleEstimator g_yawScale;
ScaleEstimator g_pitchScale;
std::array<AxisLayout, 2> g_layouts{AxisLayout{false}, AxisLayout{true}};
int g_chosenLayout = -1;
std::array<float, 16> g_previousView{};
bool g_hasPreviousView = false;
Vec3 g_latestRight{};
bool g_hasLatestRight = false;

std::int64_t g_tickConsumedX = 0;
std::int64_t g_tickConsumedY = 0;
float g_lastYaw = 0.0F;
float g_lastPitch = 0.0F;
bool g_hasLastAngles = false;
double g_pitchLow = -1.55;
double g_pitchHigh = 1.55;

enum class TickResponse {
    idle,      // no counts consumed
    exact,     // the camera turned by exactly scale * counts
    none,      // counts were consumed and the camera did not turn at all
    limit,     // as `none`, but explained by the game's pitch limit
    ambiguous, // the camera turned by something else
};

// Whether the mouse is currently what turns one axis of the camera.
//
// The game's response is exact, so one tick usually settles it: a turn of
// exactly scale * counts attaches the axis, counts that leave the camera
// still detach it (an interaction has taken the mouse). Anything else is the
// mouse plus sway, recoil or an animation, or a scripted move without the
// mouse; one tick cannot tell those apart, so the axis keeps its state.
//
// A detached axis leads no new counts. Every change of state is continuous
// in what the view shows: attaching leaves the tick it happens on to plain
// interpolation, and a lead that turned out wrong is released over a few
// ticks instead of being dropped in one frame.
struct AxisTracker {
    static constexpr int kStrongMissLimit = 3;
    static constexpr double kReleaseTicks = 3.0;

    bool attached = false;
    // While detached, counts observed up to here are still led: the ones
    // that arrived before a mouse button put the axis on hold.
    std::int64_t heldFrom = 0;
    std::uint64_t attachFromSerial = 0;
    std::int64_t shownPending = 0; // pending counts led on the latest frame
    double tickTurn = 0.0; // led part of the newest tick's turn, radians
    double release = 0.0;  // a lead that was shown and has to be taken back
    std::uint64_t releaseSerial = 0;
    int strongMisses = 0;

    [[nodiscard]] static bool IsExact(const double scale,
                                      const std::int64_t counts,
                                      const double angle) noexcept {
        const double expected = scale * static_cast<double>(counts);
        return std::abs(angle - expected) <=
               0.25 * std::abs(scale) + 2.0e-3 * std::abs(expected);
    }

    [[nodiscard]] static TickResponse Classify(const double scale,
                                               const std::int64_t counts,
                                               const double angle,
                                               const bool atLimit) noexcept {
        if (counts == 0) {
            return TickResponse::idle;
        }
        if (IsExact(scale, counts, angle)) {
            return TickResponse::exact;
        }
        if (std::abs(angle) < 1.0e-6) {
            return atLimit ? TickResponse::limit : TickResponse::none;
        }
        return TickResponse::ambiguous;
    }

    // A mouse button went down: it may be starting an interaction that takes
    // the mouse away from the camera. Counts from here on are not led until
    // a tick that began after the press shows the camera still following.
    void Hold(const std::int64_t observedAtPress,
              const std::uint64_t latestTickSerial) noexcept {
        if (attached) {
            attached = false;
            heldFrom = observedAtPress;
        }
        // The next tick consumes the press itself; the one after is the
        // first whose counts all arrived later.
        attachFromSerial = latestTickSerial + 2;
    }

    // Returns true if the axis was detached by this tick.
    bool Observe(const TickResponse response,
                 const double scale,
                 const std::int64_t counts,
                 const double angle,
                 const std::int64_t consumedNow,
                 const std::uint64_t serial) noexcept {
        // What the view was showing of this tick's counts before it ran.
        const std::int64_t ledCounts = attached ? counts : shownPending;
        const double ledTurn = scale * static_cast<double>(ledCounts);
        const double expected = scale * static_cast<double>(counts);
        bool detach = false;
        switch (response) {
        case TickResponse::idle:
        case TickResponse::limit:
            strongMisses = 0;
            break;
        case TickResponse::exact:
            strongMisses = 0;
            if (serial >= attachFromSerial) {
                attached = true;
            }
            break;
        case TickResponse::none:
            detach = true;
            break;
        case TickResponse::ambiguous:
            // A large turn the camera opposed or all but ignored, several
            // ticks running: a scripted move, not an effect on top of the
            // mouse.
            if (std::abs(expected) > 0.02 &&
                (angle * expected <= 0.0 ||
                 std::abs(angle) < 0.25 * std::abs(expected))) {
                detach = ++strongMisses >= kStrongMissLimit;
            } else {
                strongMisses = 0;
            }
            break;
        }
        const bool wasAttached = attached;
        if (detach) {
            attached = false;
            strongMisses = 0;
            tickTurn = 0.0;
            if (ledCounts != 0) {
                release = ledTurn;
                releaseSerial = serial;
            }
        } else {
            tickTurn = ledTurn;
        }
        if (!attached) {
            heldFrom = consumedNow;
        }
        shownPending = 0;
        return detach && wasAttached;
    }

    // The camera was replaced, cut, or is not in a mode the fields describe.
    void Reset(const std::int64_t consumedNow) noexcept {
        attached = false;
        heldFrom = consumedNow;
        shownPending = 0;
        tickTurn = 0.0;
        release = 0.0;
        strongMisses = 0;
    }

    [[nodiscard]] double Lead(const double scale,
                              const std::int64_t observed,
                              const std::int64_t consumed,
                              const std::uint64_t serial,
                              const double alpha) noexcept {
        shownPending = (attached ? observed : heldFrom) - consumed;
        double lead = tickTurn * (1.0 - alpha) +
                      scale * static_cast<double>(shownPending);
        if (release != 0.0) {
            const double remaining =
                1.0 - (static_cast<double>(serial - releaseSerial) + alpha) /
                          kReleaseTicks;
            if (serial >= releaseSerial && remaining > 0.0) {
                lead += release * remaining;
            } else {
                release = 0.0;
            }
        }
        return lead;
    }
};

AxisTracker g_yawAxis;
AxisTracker g_pitchAxis;
std::uint64_t g_leadSerial = 0;
std::uint32_t g_seenPresses = 0;
double g_tickYawDelta = 0.0; // the newest tick's whole yaw change
double g_leadYawBack = 0.0;  // interpolated yaw minus the newest tick's yaw
Report::Miss g_misses[Report::kMissCapacity]{};
std::size_t g_missCount = 0;

void RecordMiss(const wchar_t axis,
                const std::int64_t counts,
                const double angle,
                const double expected) noexcept {
    if (g_missCount < Report::kMissCapacity) {
        g_misses[g_missCount++] = {axis, counts, angle, expected};
    }
}

void ApplyPresses() noexcept {
    const std::uint32_t presses = g_pressCount.load(std::memory_order_acquire);
    if (presses == g_seenPresses) {
        return;
    }
    g_seenPresses = presses;
    g_yawAxis.Hold(g_pressObservedX.load(std::memory_order_relaxed),
                   g_leadSerial);
    g_pitchAxis.Hold(g_pressObservedY.load(std::memory_order_relaxed),
                     g_leadSerial);
}

std::uint64_t g_detaches = 0;
std::uint64_t g_ticks = 0;
std::uint64_t g_ticksWithMotion = 0;
std::uint64_t g_turnsWithoutMotion = 0;
std::uint64_t g_leadFrames = 0;
std::uint64_t g_mismatchTicks = 0;
std::uint64_t g_suspendedTicks = 0;
std::uint64_t g_clampedTicks = 0;
std::uint64_t g_scaleChanges = 0;
std::int64_t g_reportObservedX = 0;
std::int64_t g_reportObservedY = 0;
std::int64_t g_reportConsumedX = 0;
std::int64_t g_reportConsumedY = 0;

double WrapAngle(double angle) noexcept {
    while (angle > kPi) {
        angle -= 2.0 * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0 * kPi;
    }
    return angle;
}

int EventWatch(void*, void* const event) {
    if (event == nullptr) {
        return 1;
    }
    std::uint32_t type = 0;
    std::memcpy(&type, event, sizeof(type));
    if (type == kSdlMouseButtonDown) {
        g_pressObservedX.store(g_observedX.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        g_pressObservedY.store(g_observedY.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        g_pressCount.fetch_add(1, std::memory_order_release);
        return 1;
    }
    if (type != kSdlMouseMotion) {
        return 1;
    }
    std::int32_t xrel = 0;
    std::int32_t yrel = 0;
    std::memcpy(&xrel, static_cast<const std::uint8_t*>(event) +
                           kMotionXRelOffset,
                sizeof(xrel));
    std::memcpy(&yrel, static_cast<const std::uint8_t*>(event) +
                           kMotionYRelOffset,
                sizeof(yrel));
    g_observedX.fetch_add(xrel, std::memory_order_relaxed);
    g_observedY.fetch_add(yrel, std::memory_order_relaxed);
    g_motionEvents.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (g_lastMotionFrame.exchange(frame, std::memory_order_relaxed) !=
        frame) {
        g_motionFrames.fetch_add(1, std::memory_order_relaxed);
    }
    return 1;
}

std::uint32_t HookedGetRelativeMouseState(int* const x, int* const y) {
    if (!g_watchAdded.exchange(true, std::memory_order_acq_rel)) {
        // The first consume call proves SDL's event subsystem is running.
        // Counting starts from here on both sides.
        g_addEventWatch(&EventWatch, nullptr);
        const std::uint32_t buttons = g_originalGetRelativeMouseState(x, y);
        g_observedX.store(0, std::memory_order_relaxed);
        g_observedY.store(0, std::memory_order_relaxed);
        return buttons;
    }
    int localX = 0;
    int localY = 0;
    const std::uint32_t buttons =
        g_originalGetRelativeMouseState(&localX, &localY);
    if (x != nullptr) {
        *x = localX;
    }
    if (y != nullptr) {
        *y = localY;
    }
    g_consumeCalls.fetch_add(1, std::memory_order_relaxed);
    if (localX != 0 || localY != 0) {
        g_consumeNonZero.fetch_add(1, std::memory_order_relaxed);
    }
    const std::int64_t consumedX =
        g_consumedX.fetch_add(localX, std::memory_order_relaxed) + localX;
    const std::int64_t consumedY =
        g_consumedY.fetch_add(localY, std::memory_order_relaxed) + localY;
    const std::int64_t drift =
        std::llabs(g_observedX.load(std::memory_order_relaxed) - consumedX) +
        std::llabs(g_observedY.load(std::memory_order_relaxed) - consumedY);
    std::int64_t seen = g_maximumDrift.load(std::memory_order_relaxed);
    while (seen < drift &&
           !g_maximumDrift.compare_exchange_weak(seen, drift,
                                                 std::memory_order_relaxed)) {
    }
    return buttons;
}

void LearnAxes(const float* const view,
               const double yawDelta,
               const double pitchDelta) noexcept {
    if (g_chosenLayout >= 0 || !g_hasPreviousView) {
        return;
    }
    const bool yawMoved = std::abs(yawDelta) > kMinimumTurn;
    const bool pitchMoved = std::abs(pitchDelta) > kMinimumTurn;
    if ((!yawMoved && !pitchMoved) || std::abs(yawDelta) > kMaximumTurn ||
        std::abs(pitchDelta) > kMaximumTurn) {
        return;
    }
    for (AxisLayout& layout : g_layouts) {
        const Vec3 rightBefore = layout.Right(g_previousView.data());
        const Vec3 upBefore = layout.Up(g_previousView.data());
        const Vec3 rightNow = layout.Right(view);
        const Vec3 upNow = layout.Up(view);
        double best = -2.0;
        double bestYawSign = 1.0;
        double bestPitchSign = 1.0;
        for (const double yawSign : {1.0, -1.0}) {
            for (const double pitchSign : {1.0, -1.0}) {
                const Vec3 right =
                    RotateAboutY(rightBefore, yawSign * yawDelta);
                const Vec3 up = RotateAboutY(
                    RotateAboutAxis(upBefore, rightBefore,
                                    pitchSign * pitchDelta),
                    yawSign * yawDelta);
                const double score =
                    std::min(Dot(right, rightNow), Dot(up, upNow));
                if (score > best) {
                    best = score;
                    bestYawSign = yawSign;
                    bestPitchSign = pitchSign;
                }
            }
        }
        if (best < kAgreement) {
            ++layout.unexplained;
            continue;
        }
        if (yawMoved) {
            ++(bestYawSign > 0.0 ? layout.yawPositive : layout.yawNegative);
        }
        if (pitchMoved) {
            ++(bestPitchSign > 0.0 ? layout.pitchPositive
                                   : layout.pitchNegative);
        }
    }
    for (std::size_t index = 0; index < g_layouts.size(); ++index) {
        if (g_layouts[index].Ready()) {
            g_chosenLayout = static_cast<int>(index);
            break;
        }
    }
}

} // namespace

bool InstallTrace(std::wstring& error) noexcept {
    error.clear();
    const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
    if (sdl == nullptr) {
        error = L"SDL2.dll is not loaded";
        return false;
    }
    void* const getRelative = reinterpret_cast<void*>(
        GetProcAddress(sdl, "SDL_GetRelativeMouseState"));
    g_addEventWatch = reinterpret_cast<SdlAddEventWatch>(
        GetProcAddress(sdl, "SDL_AddEventWatch"));
    g_pumpEvents = reinterpret_cast<SdlPumpEvents>(
        GetProcAddress(sdl, "SDL_PumpEvents"));
    // Only the GUI cursor needs these; it stays off without them.
    g_getMouseState = reinterpret_cast<SdlGetMouseState>(
        GetProcAddress(sdl, "SDL_GetMouseState"));
    g_getRelativeMouseMode = reinterpret_cast<SdlGetRelativeMouseMode>(
        GetProcAddress(sdl, "SDL_GetRelativeMouseMode"));
    if (getRelative == nullptr || g_addEventWatch == nullptr ||
        g_pumpEvents == nullptr) {
        error = L"SDL2.dll does not export the expected mouse functions";
        return false;
    }
    MH_STATUS status = MH_CreateHook(
        getRelative, reinterpret_cast<void*>(&HookedGetRelativeMouseState),
        reinterpret_cast<void**>(&g_originalGetRelativeMouseState));
    if (status == MH_OK) {
        status = MH_EnableHook(getRelative);
        if (status != MH_OK) {
            (void)MH_RemoveHook(getRelative);
        }
    }
    if (status != MH_OK) {
        g_originalGetRelativeMouseState = nullptr;
        error = L"Could not hook SDL_GetRelativeMouseState (MinHook status " +
                std::to_wstring(status) + L")";
        return false;
    }
    return true;
}

void ConfigurePreview(const bool enabled) noexcept {
    g_previewEnabled = enabled;
}

void PumpForFrame() noexcept {
    if (g_pumpEvents == nullptr ||
        !g_watchAdded.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (frame == g_pumpedFrame) {
        return;
    }
    g_pumpedFrame = frame;
    g_pumpEvents();
}

CursorState ReadCursor() noexcept {
    CursorState state{};
    if (g_getMouseState == nullptr || g_getRelativeMouseMode == nullptr ||
        !g_watchAdded.load(std::memory_order_relaxed)) {
        return state;
    }
    state.available = true;
    state.relativeMode = g_getRelativeMouseMode() != 0;
    (void)g_getMouseState(&state.x, &state.y);
    state.observedX = g_observedX.load(std::memory_order_relaxed);
    state.observedY = g_observedY.load(std::memory_order_relaxed);
    return state;
}

void OnPresent() noexcept {
    g_frame.fetch_add(1, std::memory_order_relaxed);
}

void OnCameraTick(const std::uint64_t serial,
                  const float yaw,
                  const float pitch,
                  const float* const view,
                  const bool valid) noexcept {
    const std::int64_t consumedX = g_consumedX.load(std::memory_order_relaxed);
    const std::int64_t consumedY = g_consumedY.load(std::memory_order_relaxed);
    const std::int64_t countsX = consumedX - g_tickConsumedX;
    const std::int64_t countsY = consumedY - g_tickConsumedY;
    g_tickConsumedX = consumedX;
    g_tickConsumedY = consumedY;
    ApplyPresses();
    g_leadSerial = serial;
    g_tickYawDelta = 0.0;

    const bool usable = valid && std::isfinite(yaw) && std::isfinite(pitch);
    if (usable) {
        // A learned limit the camera has since moved past was not a limit.
        if (pitch > g_pitchHigh + 1.0e-3) {
            g_pitchHigh = 1.55;
        }
        if (pitch < g_pitchLow - 1.0e-3) {
            g_pitchLow = -1.55;
        }
    }
    if (usable && g_hasLastAngles) {
        const double yawDelta = WrapAngle(static_cast<double>(yaw) - g_lastYaw);
        const double pitchDelta =
            WrapAngle(static_cast<double>(pitch) - g_lastPitch);
        ++g_ticks;
        if (countsX != 0 || countsY != 0) {
            ++g_ticksWithMotion;
        } else if (std::abs(yawDelta) > 1.0e-6 ||
                   std::abs(pitchDelta) > 1.0e-6) {
            ++g_turnsWithoutMotion;
        }
        if (view != nullptr) {
            LearnAxes(view, yawDelta, pitchDelta);
        }
        if (g_yawScale.Observe(countsX, yawDelta)) {
            ++g_scaleChanges;
        }
        if (g_pitchScale.Observe(countsY, pitchDelta)) {
            ++g_scaleChanges;
        }

        g_tickYawDelta = yawDelta;
        TickResponse yawResponse = TickResponse::idle;
        if (g_yawScale.ready) {
            yawResponse = AxisTracker::Classify(g_yawScale.value, countsX,
                                                yawDelta, false);
            if (g_yawAxis.Observe(yawResponse, g_yawScale.value, countsX,
                                  yawDelta, consumedX, serial)) {
                ++g_detaches;
                RecordMiss(L'Y', countsX, yawDelta,
                           g_yawScale.value * static_cast<double>(countsX));
            }
        } else {
            g_yawAxis.Reset(consumedX);
        }
        TickResponse pitchResponse = TickResponse::idle;
        if (g_pitchScale.ready) {
            // Counts that leave the pitch still are the game's limit, not an
            // interaction, if the same tick's yaw followed the mouse or the
            // camera is resting on a limit already found that way.
            const double expectedPitch =
                g_pitchScale.value * static_cast<double>(countsY);
            const bool yawFollowed = yawResponse == TickResponse::exact;
            const bool atKnownLimit =
                std::abs(pitch - (expectedPitch > 0.0 ? g_pitchHigh
                                                      : g_pitchLow)) < 1.0e-3;
            pitchResponse = AxisTracker::Classify(
                g_pitchScale.value, countsY, pitchDelta,
                yawFollowed || atKnownLimit);
            if (pitchResponse == TickResponse::limit) {
                ++g_clampedTicks;
                if (expectedPitch > 0.0) {
                    g_pitchHigh = pitch;
                } else {
                    g_pitchLow = pitch;
                }
            }
            if (g_pitchAxis.Observe(pitchResponse, g_pitchScale.value, countsY,
                                    pitchDelta, consumedY, serial)) {
                ++g_detaches;
                RecordMiss(L'P', countsY, pitchDelta, expectedPitch);
            }
        } else {
            g_pitchAxis.Reset(consumedY);
        }
        if (yawResponse == TickResponse::ambiguous ||
            pitchResponse == TickResponse::ambiguous) {
            ++g_mismatchTicks;
        }
        if ((countsX != 0 && !g_yawAxis.attached) ||
            (countsY != 0 && !g_pitchAxis.attached)) {
            ++g_suspendedTicks;
        }
    } else {
        g_yawAxis.Reset(consumedX);
        g_pitchAxis.Reset(consumedY);
    }
    if (usable && view != nullptr) {
        std::memcpy(g_previousView.data(), view, sizeof(g_previousView));
        g_hasPreviousView = true;
        if (g_chosenLayout >= 0) {
            g_latestRight =
                g_layouts[static_cast<std::size_t>(g_chosenLayout)].Right(view);
            g_hasLatestRight = true;
        }
    } else {
        g_hasPreviousView = false;
        g_hasLatestRight = false;
    }
    g_lastYaw = yaw;
    g_lastPitch = pitch;
    g_hasLastAngles = usable;
}

Lead ComputeLead(const std::uint64_t newerSerial, const double alpha) noexcept {
    Lead lead{};
    if (!g_previewEnabled || g_pumpEvents == nullptr ||
        !g_watchAdded.load(std::memory_order_relaxed)) {
        return lead;
    }
    // The game pumps SDL once per fixed tick; without this, motion that
    // arrived since then would stay invisible until the next tick. Events are
    // only moved into SDL's queue here, never taken out of it.
    PumpForFrame();
    ApplyPresses();
    if (newerSerial != g_leadSerial || g_chosenLayout < 0 ||
        !g_hasLatestRight || !std::isfinite(alpha) || alpha < 0.0 ||
        alpha > 1.0) {
        return lead;
    }
    // Each axis leads on its own: a pitch limit or a pitch effect must not
    // take the horizontal response away, and the other way round.
    const double yaw = g_yawAxis.Lead(
        g_yawScale.value, g_observedX.load(std::memory_order_relaxed),
        g_consumedX.load(std::memory_order_relaxed), newerSerial, alpha);
    const double pitch = g_pitchAxis.Lead(
        g_pitchScale.value, g_observedY.load(std::memory_order_relaxed),
        g_consumedY.load(std::memory_order_relaxed), newerSerial, alpha);
    if (!std::isfinite(yaw) || !std::isfinite(pitch) ||
        std::abs(yaw) > kMaximumLead || std::abs(pitch) > kMaximumLead) {
        return lead;
    }
    const bool leading = g_yawAxis.attached || g_pitchAxis.attached;
    if (!leading && yaw == 0.0 && pitch == 0.0) {
        return lead;
    }
    // The interpolated camera has not finished the newest tick's yaw, and
    // its right axis is that much behind the one read at the tick.
    g_leadYawBack = -(1.0 - alpha) * g_tickYawDelta;
    lead.active = true;
    lead.yaw = static_cast<float>(yaw);
    lead.pitch = static_cast<float>(pitch);
    lead.pitchLow = static_cast<float>(g_pitchLow);
    lead.pitchHigh = static_cast<float>(g_pitchHigh);
    if (leading) {
        ++g_leadFrames;
    }
    return lead;
}

bool BuildLeadRotation(const float yaw,
                       const float pitch,
                       float rotation[9]) noexcept {
    if (rotation == nullptr || g_chosenLayout < 0 || !g_hasLatestRight ||
        !std::isfinite(yaw) || !std::isfinite(pitch)) {
        return false;
    }
    const AxisLayout& layout =
        g_layouts[static_cast<std::size_t>(g_chosenLayout)];
    const double yawAngle = layout.YawSign() * yaw;
    const double pitchAngle = layout.PitchSign() * pitch;
    // Pitch turns about the interpolated camera's right axis, then yaw about
    // world up.
    const Vec3 right =
        RotateAboutY(g_latestRight, layout.YawSign() * g_leadYawBack);
    const std::array<Vec3, 3> basis{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0},
                                    Vec3{0.0, 0.0, 1.0}};
    for (std::size_t column = 0; column < 3; ++column) {
        const Vec3 turned = RotateAboutY(
            RotateAboutAxis(basis[column], right, pitchAngle), yawAngle);
        rotation[column] = static_cast<float>(turned.x);
        rotation[3 + column] = static_cast<float>(turned.y);
        rotation[6 + column] = static_cast<float>(turned.z);
    }
    for (std::size_t index = 0; index < 9; ++index) {
        if (!std::isfinite(rotation[index])) {
            return false;
        }
    }
    return true;
}

Report TakeReport() noexcept {
    Report report{};
    report.motionEvents = g_motionEvents.exchange(0, std::memory_order_acq_rel);
    report.motionFrames = g_motionFrames.exchange(0, std::memory_order_acq_rel);
    report.consumeCalls = g_consumeCalls.exchange(0, std::memory_order_acq_rel);
    report.consumeNonZero =
        g_consumeNonZero.exchange(0, std::memory_order_acq_rel);
    const std::int64_t observedX = g_observedX.load(std::memory_order_relaxed);
    const std::int64_t observedY = g_observedY.load(std::memory_order_relaxed);
    const std::int64_t consumedX = g_consumedX.load(std::memory_order_relaxed);
    const std::int64_t consumedY = g_consumedY.load(std::memory_order_relaxed);
    report.observedX = observedX - g_reportObservedX;
    report.observedY = observedY - g_reportObservedY;
    report.consumedX = consumedX - g_reportConsumedX;
    report.consumedY = consumedY - g_reportConsumedY;
    g_reportObservedX = observedX;
    g_reportObservedY = observedY;
    g_reportConsumedX = consumedX;
    g_reportConsumedY = consumedY;
    report.maximumDrift = g_maximumDrift.load(std::memory_order_relaxed);
    report.ticks = g_ticks;
    report.ticksWithMotion = g_ticksWithMotion;
    report.turnsWithoutMotion = g_turnsWithoutMotion;
    report.scaleReady = g_yawScale.ready && g_pitchScale.ready;
    report.axesReady = g_chosenLayout >= 0;
    for (std::size_t index = 0; index < g_layouts.size(); ++index) {
        const AxisLayout& layout = g_layouts[index];
        report.axisVotes[index][0] = layout.yawPositive;
        report.axisVotes[index][1] = layout.yawNegative;
        report.axisVotes[index][2] = layout.pitchPositive;
        report.axisVotes[index][3] = layout.pitchNegative;
        report.axisVotes[index][4] = layout.unexplained;
    }
    report.yawPerCount = g_yawScale.value;
    report.pitchPerCount = g_pitchScale.value;
    report.pitchLow = g_pitchLow;
    report.pitchHigh = g_pitchHigh;
    report.leadFrames = g_leadFrames;
    report.mismatchTicks = g_mismatchTicks;
    report.suspendedTicks = g_suspendedTicks;
    report.clampedTicks = g_clampedTicks;
    report.scaleChanges = g_scaleChanges;
    report.detaches = g_detaches;
    g_detaches = 0;
    report.missCount = g_missCount;
    for (std::size_t index = 0; index < g_missCount; ++index) {
        report.misses[index] = g_misses[index];
    }
    g_missCount = 0;
    g_ticks = 0;
    g_ticksWithMotion = 0;
    g_turnsWithoutMotion = 0;
    g_leadFrames = 0;
    g_mismatchTicks = 0;
    g_suspendedTicks = 0;
    g_clampedTicks = 0;
    g_scaleChanges = 0;
    return report;
}

} // namespace atb::mouse
