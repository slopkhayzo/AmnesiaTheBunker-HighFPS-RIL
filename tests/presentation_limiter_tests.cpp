#include "presentation_limiter.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

bool Check(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    atb::limiter::DeadlineSchedule schedule;
    if (!Check(!schedule.Configure(10'000'000, 0),
               "zero target was accepted") ||
        !Check(!schedule.Configure(999, 1000),
               "target above timer resolution was accepted") ||
        !Check(schedule.Configure(10'000'000, 240),
               "valid 240 Hz schedule was rejected")) {
        return EXIT_FAILURE;
    }

    constexpr std::int64_t start = 1'000'000;
    auto plan = schedule.BeforePresent(start);
    if (!Check(!plan.shouldWait && plan.deadline == start,
               "first present should establish phase without waiting")) {
        return EXIT_FAILURE;
    }
    schedule.Presented(start);

    plan = schedule.BeforePresent(start + 10'000);
    if (!Check(plan.shouldWait && plan.deadline == 1'041'666,
               "second present deadline is incorrect")) {
        return EXIT_FAILURE;
    }
    schedule.Presented(plan.deadline);

    // The fractional accumulator must produce exactly one second of deadlines,
    // rather than drifting because 10 MHz is not evenly divisible by 240.
    std::int64_t deadline = plan.deadline;
    for (unsigned int frame = 2; frame < 240; ++frame) {
        plan = schedule.BeforePresent(deadline);
        deadline = plan.deadline;
        schedule.Presented(deadline);
    }
    plan = schedule.BeforePresent(deadline);
    if (!Check(plan.deadline == start + 10'000'000,
               "fractional schedule drifted over one second")) {
        return EXIT_FAILURE;
    }

    plan = schedule.BeforePresent(plan.deadline + 3'000'000);
    if (!Check(plan.phaseReset && !plan.shouldWait,
               "long suspension did not reset limiter phase")) {
        return EXIT_FAILURE;
    }
    schedule.Presented(plan.deadline);
    plan = schedule.BeforePresent(plan.deadline + 1);
    if (!Check(plan.shouldWait,
               "schedule did not resume after a phase reset")) {
        return EXIT_FAILURE;
    }

    std::cout << "presentation limiter schedule tests passed\n";
    return EXIT_SUCCESS;
}
