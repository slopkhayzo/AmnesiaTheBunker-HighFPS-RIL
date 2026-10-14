#include "presentation_limiter.h"

#include <algorithm>

namespace atb::limiter {

bool DeadlineSchedule::Configure(const std::int64_t counterFrequency,
                                 const unsigned int targetFps) noexcept {
    Reset();
    if (counterFrequency <= 0 || targetFps == 0 ||
        static_cast<std::uint64_t>(counterFrequency) < targetFps) {
        return false;
    }
    frequency_ = counterFrequency;
    targetFps_ = targetFps;
    basePeriod_ = counterFrequency / static_cast<std::int64_t>(targetFps);
    periodRemainder_ = static_cast<std::uint64_t>(counterFrequency) % targetFps;
    lateResetThreshold_ = std::max<std::int64_t>(
        basePeriod_ * 4, counterFrequency / 4);
    return true;
}

WaitPlan DeadlineSchedule::BeforePresent(const std::int64_t now) noexcept {
    if (frequency_ <= 0 || nextDeadline_ == 0) {
        return {now, false, false};
    }
    if (now > nextDeadline_ &&
        now - nextDeadline_ > lateResetThreshold_) {
        nextDeadline_ = 0;
        remainderAccumulator_ = 0;
        return {now, false, true};
    }
    return {nextDeadline_, now < nextDeadline_, false};
}

void DeadlineSchedule::Presented(const std::int64_t now) noexcept {
    if (frequency_ <= 0) {
        return;
    }
    if (nextDeadline_ == 0) {
        nextDeadline_ = now;
        AdvanceDeadline();
        return;
    }
    while (nextDeadline_ <= now) {
        AdvanceDeadline();
    }
}

void DeadlineSchedule::Reset() noexcept {
    frequency_ = 0;
    basePeriod_ = 0;
    lateResetThreshold_ = 0;
    nextDeadline_ = 0;
    periodRemainder_ = 0;
    remainderAccumulator_ = 0;
    targetFps_ = 0;
}

void DeadlineSchedule::AdvanceDeadline() noexcept {
    nextDeadline_ += basePeriod_;
    remainderAccumulator_ += periodRemainder_;
    if (remainderAccumulator_ >= targetFps_) {
        ++nextDeadline_;
        remainderAccumulator_ -= targetFps_;
    }
}

} // namespace atb::limiter
