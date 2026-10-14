#pragma once

#include <cstdint>

namespace atb::limiter {

struct WaitPlan {
    std::int64_t deadline = 0;
    bool shouldWait = false;
    bool phaseReset = false;
};

class DeadlineSchedule {
public:
    [[nodiscard]] bool Configure(std::int64_t counterFrequency,
                                 unsigned int targetFps) noexcept;
    [[nodiscard]] WaitPlan BeforePresent(std::int64_t now) noexcept;
    void Presented(std::int64_t now) noexcept;
    void Reset() noexcept;

private:
    void AdvanceDeadline() noexcept;

    std::int64_t frequency_ = 0;
    std::int64_t basePeriod_ = 0;
    std::int64_t lateResetThreshold_ = 0;
    std::int64_t nextDeadline_ = 0;
    std::uint64_t periodRemainder_ = 0;
    std::uint64_t remainderAccumulator_ = 0;
    unsigned int targetFps_ = 0;
};

} // namespace atb::limiter
