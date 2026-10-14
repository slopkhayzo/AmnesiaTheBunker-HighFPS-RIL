#include "frame_pacing.h"

#include <algorithm>
#include <cstdlib>

namespace atb::pacing {

void Pacer::Ring::Push(const std::int64_t value) noexcept {
    values[next] = value;
    next = (next + 1) % kSamples;
    count = std::min(count + 1, kSamples);
}

std::int64_t Pacer::Ring::Percentile(const unsigned int percent) const noexcept {
    if (count == 0) {
        return 0;
    }
    std::array<std::int64_t, kSamples> sorted = values;
    const std::size_t index = std::min(count - 1, count * percent / 100);
    std::nth_element(sorted.begin(), sorted.begin() + index,
                     sorted.begin() + count);
    return sorted[index];
}

bool Pacer::Configure(const std::int64_t counterFrequency,
                      const std::int64_t minimumPeriod,
                      const unsigned int evenness) noexcept {
    Reset();
    if (counterFrequency <= 0 || minimumPeriod <= 0 || evenness == 0 ||
        evenness > 100) {
        return false;
    }
    frequency_ = counterFrequency;
    minimumPeriod_ = minimumPeriod;
    tolerancePercent_ = 100 - evenness;
    return true;
}

void Pacer::Reset() noexcept { *this = Pacer{}; }

unsigned int Pacer::Cap() const noexcept {
    return static_cast<unsigned int>(std::clamp<std::int64_t>(
        tickPeriod_ / minimumPeriod_, 1, kMaximumFramesPerTick));
}

std::int64_t Pacer::TickLengthFor(const unsigned int frames,
                                  const std::int64_t tickWork) const noexcept {
    // Never shorter than an even share: evenness only ever lengthens the
    // tick frame, it does not shorten it below the others.
    return frames <= 1 ? tickPeriod_
                       : std::max(tickWork, tickPeriod_ / frames);
}

bool Pacer::Fits(const unsigned int frames,
                 const std::int64_t tickWork,
                 const std::int64_t plainWork,
                 const unsigned int tolerancePercent) const noexcept {
    if (frames <= 1) {
        return true;
    }
    const std::int64_t tickLength = TickLengthFor(frames, tickWork);
    const std::int64_t plainLength =
        (tickPeriod_ - tickLength) / static_cast<std::int64_t>(frames - 1);
    return plainLength >= std::max(minimumPeriod_, plainWork) &&
           tickLength * 100 <= plainLength * (100 + tolerancePercent) +
                                   plainLength / 100;
}

unsigned int Pacer::Most(unsigned int from,
                         const std::int64_t tickWork,
                         const std::int64_t plainWork,
                         const unsigned int tolerancePercent) const noexcept {
    while (from > 1 && !Fits(from, tickWork, plainWork, tolerancePercent)) {
        --from;
    }
    return from;
}

std::int64_t Pacer::PlainFrameLength() const noexcept {
    return framesPerTick_ <= 1
               ? tickPeriod_
               : (tickPeriod_ - tickLength_) /
                     static_cast<std::int64_t>(framesPerTick_ - 1);
}

Plan Pacer::BeginFrame(const std::int64_t now,
                       const std::int64_t tickDue,
                       const std::int64_t tickPeriod) noexcept {
    // Between 5 and 1000 ticks per second, and a due time near the present:
    // anything else is a loading screen, a stall, or a timer not yet running.
    if (frequency_ <= 0 || tickPeriod < frequency_ / 1000 ||
        tickPeriod > frequency_ / 5 ||
        std::abs(tickDue - now) > 2 * tickPeriod) {
        lastDeadline_ = 0;
        return {};
    }
    if (tickPeriod != tickPeriod_) {
        // A different simulation rate: start from the cap and measure again.
        tickPeriod_ = tickPeriod;
        framesPerTick_ = Cap();
        tickLength_ = tickPeriod_ / framesPerTick_;
        measured_ = false;
        lowerTicks_ = 0;
        raiseTicks_ = 0;
        tickBusy_ = {};
        plainBusy_ = {};
    }
    const std::int64_t plainLength = PlainFrameLength();
    // Presents sit a little after the grid so that the frame following the
    // one presented on a tick's due time always finds the tick due.
    const std::int64_t offset = frequency_ / 4000;
    const bool continues = lastDeadline_ != 0 &&
                           now - lastDeadline_ < 4 * tickPeriod &&
                           lastDeadline_ - now < tickPeriod;
    // The next slot is the first one clearly after the previous present.
    const std::int64_t after =
        continues ? lastDeadline_ + plainLength / 2 : now;
    // Slots of the tick periods around now, in order: the tick frame's
    // present, then one per plain frame; the last falls on the next due time.
    std::int64_t chosen = 0;
    std::int64_t following = 0;
    for (int period = -2; period <= 1 && following == 0; ++period) {
        const std::int64_t begin = tickDue + period * tickPeriod + offset;
        for (unsigned int slot = 1; slot <= framesPerTick_; ++slot) {
            const std::int64_t at =
                slot == framesPerTick_
                    ? begin + tickPeriod
                    : begin + tickLength_ +
                          static_cast<std::int64_t>(slot - 1) * plainLength;
            if (chosen == 0) {
                if (at > after) {
                    chosen = at;
                }
            } else if (now + renderLead_ > at) {
                // Too late even for the slot after it: skip ahead.
                chosen = at;
            } else {
                following = at;
                break;
            }
        }
    }
    if (chosen == 0) {
        lastDeadline_ = 0;
        return {};
    }
    Plan plan{};
    plan.valid = true;
    plan.deadline = chosen;
    plan.renderStart = chosen - renderLead_;
    lastDeadline_ = chosen;
    return plan;
}

void Pacer::EndFrame(const std::int64_t busy,
                     const std::int64_t render,
                     const unsigned int ticks) noexcept {
    if (tickPeriod_ <= 0 || busy <= 0 || busy > 2 * tickPeriod_ || ticks > 1) {
        return; // a stall or a catch-up burst says nothing about pacing
    }
    if (render > 0 && render <= busy) {
        render_.Push(render);
    }
    if (ticks == 0) {
        plainBusy_.Push(busy);
        return;
    }
    tickBusy_.Push(busy);
    Adapt();
}

void Pacer::Adapt() noexcept {
    // Start the render early enough for nearly every frame to make its
    // present; grow at once, shrink slowly.
    const std::int64_t lead = render_.Percentile(95) + frequency_ / 10000;
    renderLead_ = lead > renderLead_ ? lead
                                     : renderLead_ + (lead - renderLead_) / 16;

    if (tickBusy_.count < kMinimumSamples) {
        return;
    }
    // Room for three out of four tick frames, plus a small margin.
    const std::int64_t tickWork =
        tickBusy_.Percentile(75) + frequency_ / 10000;
    const std::int64_t plainWork = plainBusy_.Percentile(75);
    const unsigned int cap = Cap();
    framesPerTick_ = std::clamp(framesPerTick_, 1U, cap);
    // Frames are added within the chosen tolerance and removed only beyond
    // twice that: in between, the tick frame just stretches.
    const unsigned int addWithin = tolerancePercent_;
    const unsigned int removeBeyond = 2 * tolerancePercent_;
    const std::int64_t spare = tickWork + tickWork * 3 / 100;

    if (!measured_) {
        measured_ = true;
        framesPerTick_ = Most(cap, tickWork, plainWork, addWithin);
    } else if (!Fits(framesPerTick_, tickWork, plainWork, removeBeyond)) {
        raiseTicks_ = 0;
        if (++lowerTicks_ >= kLowerAfterTicks) {
            lowerTicks_ = 0;
            framesPerTick_ =
                Most(framesPerTick_, tickWork, plainWork, removeBeyond);
        }
    } else {
        lowerTicks_ = 0;
        // More frames only after a second in which they would have fitted
        // with a little to spare.
        const unsigned int most = Most(cap, spare, plainWork, addWithin);
        if (most > framesPerTick_) {
            if (++raiseTicks_ >= kRaiseAfterTicks) {
                framesPerTick_ = most;
                raiseTicks_ = 0;
            }
        } else {
            raiseTicks_ = 0;
        }
    }
    // Follow the measured cost a quarter of the way per tick.
    const std::int64_t target = TickLengthFor(framesPerTick_, tickWork);
    tickLength_ += (target - tickLength_) / 4;
    const std::int64_t even = tickPeriod_ / framesPerTick_;
    const std::int64_t longest =
        framesPerTick_ <= 1
            ? tickPeriod_
            : tickPeriod_ - static_cast<std::int64_t>(framesPerTick_ - 1) *
                                minimumPeriod_;
    tickLength_ = std::clamp(tickLength_, even, std::max(even, longest));
}

} // namespace atb::pacing
