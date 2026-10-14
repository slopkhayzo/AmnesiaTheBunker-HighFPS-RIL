#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Frame pacing over a fixed-rate simulation.
//
// The frame that runs a simulation tick costs an update more than the
// others. A plain frame-rate cap therefore presents one long frame per tick
// and the view visibly skips. This pacer lays out each tick period as one
// tick frame followed by a whole number of equal plain frames, on a grid
// locked to the simulation's own timer so the tick always falls due at the
// start of a frame. How much longer than the plain frames the tick frame may
// be is the caller's choice: none at all gives perfectly even frames at a
// lower rate, more gives a higher rate with a smaller, regular unevenness.
// The pacer presents as many frames per tick as that choice, the measured
// cost of the tick frame, and the FPS cap allow.
//
// When a tick gets more expensive the tick frame is stretched first and the
// plain frames shortened to match, which changes frame times gradually. A
// frame per tick is only given up once the tick frame is twice as far over
// the others as the caller's choice allows, so the count does not flap.
//
// It also says when the render should start, so every frame samples its view
// the same time before it is presented.
//
// All times are in performance-counter ticks. No allocation, no OS calls.
namespace atb::pacing {

struct Plan {
    bool valid = false;
    std::int64_t renderStart = 0; // do not start rendering before this
    std::int64_t deadline = 0;    // present (enter the swap) at this instant
};

class Pacer {
public:
    // `minimumPeriod` is the shortest frame the user's FPS cap allows.
    // `evenness` is 1..100: frames are added while the tick frame stays
    // within (100 - evenness) percent of the plain frames' length, and
    // removed once it is more than twice that over.
    [[nodiscard]] bool Configure(std::int64_t counterFrequency,
                                 std::int64_t minimumPeriod,
                                 unsigned int evenness) noexcept;
    void Reset() noexcept;

    // Once per frame before it renders. `tickDue` is when the next tick
    // falls due (it may already have passed) and `tickPeriod` its length.
    // The plan is invalid when the simulation timer is unusable; the caller
    // then paces the frame some other way.
    [[nodiscard]] Plan BeginFrame(std::int64_t now,
                                  std::int64_t tickDue,
                                  std::int64_t tickPeriod) noexcept;

    // Once per frame at its present. `busy` is the frame's length without
    // the pacer's own waits, `render` the time from the start of its render
    // to the present (zero if unknown), `ticks` the updates it ran.
    void EndFrame(std::int64_t busy,
                  std::int64_t render,
                  unsigned int ticks) noexcept;

    [[nodiscard]] unsigned int FramesPerTick() const noexcept {
        return framesPerTick_;
    }
    [[nodiscard]] std::int64_t TickFrameLength() const noexcept {
        return tickLength_;
    }
    [[nodiscard]] std::int64_t PlainFrameLength() const noexcept;
    [[nodiscard]] std::int64_t RenderLead() const noexcept {
        return renderLead_;
    }

private:
    static constexpr std::size_t kSamples = 64;
    static constexpr unsigned int kMinimumSamples = 16;
    static constexpr unsigned int kMaximumFramesPerTick = 16;
    static constexpr unsigned int kLowerAfterTicks = 30;
    static constexpr unsigned int kRaiseAfterTicks = 60;

    struct Ring {
        std::array<std::int64_t, kSamples> values{};
        std::size_t next = 0;
        std::size_t count = 0;

        void Push(std::int64_t value) noexcept;
        [[nodiscard]] std::int64_t Percentile(unsigned int percent) const noexcept;
    };

    [[nodiscard]] unsigned int Cap() const noexcept;
    [[nodiscard]] std::int64_t TickLengthFor(unsigned int frames,
                                             std::int64_t tickWork) const noexcept;
    [[nodiscard]] bool Fits(unsigned int frames,
                            std::int64_t tickWork,
                            std::int64_t plainWork,
                            unsigned int tolerancePercent) const noexcept;
    [[nodiscard]] unsigned int Most(unsigned int from,
                                    std::int64_t tickWork,
                                    std::int64_t plainWork,
                                    unsigned int tolerancePercent) const noexcept;
    void Adapt() noexcept;

    std::int64_t frequency_ = 0;
    std::int64_t minimumPeriod_ = 0;
    unsigned int tolerancePercent_ = 0;
    std::int64_t tickPeriod_ = 0;
    std::int64_t tickLength_ = 0; // length given to the tick frame
    std::int64_t lastDeadline_ = 0;
    std::int64_t renderLead_ = 0;
    unsigned int framesPerTick_ = 0;
    bool measured_ = false; // the count has been chosen from measurements
    unsigned int lowerTicks_ = 0;
    unsigned int raiseTicks_ = 0;
    Ring tickBusy_;
    Ring plainBusy_;
    Ring render_;
};

} // namespace atb::pacing
