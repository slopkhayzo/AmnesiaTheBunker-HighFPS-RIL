#include "gui_cursor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool Near(const double left, const double right, const double tolerance) {
    return std::abs(left - right) <= tolerance;
}

// The game's own conversion for a 2560-pixel-wide window drawn into a
// 1280-unit-wide GUI that starts 40 units left of the screen.
float Sent(const int pixel) {
    return static_cast<float>(pixel) * 0.5F - 40.0F;
}

} // namespace

int main() {
    bool ok = true;
    atb::gui_cursor::AxisMap axis;
    axis.Observe(100, Sent(100));
    ok &= Require(!axis.ready, "one position should not define a mapping");
    axis.Observe(100, Sent(100));
    ok &= Require(!axis.ready, "a mouse at rest should not define a mapping");
    axis.Observe(101, Sent(101));
    ok &= Require(axis.ready && Near(axis.slope, 0.5, 1.0e-4),
                  "one pixel of movement should give a first slope");
    axis.Observe(900, Sent(900));
    ok &= Require(Near(axis.slope, 0.5, 1.0e-6),
                  "a longer move should refine the slope");
    ok &= Require(Near(axis.At(1000.0), Sent(1000), 1.0e-3) &&
                      Near(axis.At(905.5), 412.75, 1.0e-3),
                  "the mapping should reproduce the game's conversion");
    // A short move afterwards must not replace a slope measured over a long
    // one, but still re-anchors the offset.
    const double slopeBefore = axis.slope;
    axis.Observe(901, Sent(901));
    ok &= Require(axis.slope == slopeBefore &&
                      Near(axis.At(901.0), Sent(901), 1.0e-4),
                  "short moves should keep the slope and re-anchor");
    // A resolution change shows up as a clearly different slope.
    axis.Observe(1000, 1000.0F);
    axis.Observe(1010, 1010.0F);
    ok &= Require(Near(axis.slope, 1.0, 1.0e-4) &&
                      Near(axis.At(1500.0), 1500.0, 1.0e-2),
                  "a different slope should replace the old one");
    axis.Observe(1011, std::nanf(""));
    ok &= Require(!axis.ready && !axis.hasLast,
                  "a non-finite position should clear the mapping");

    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "gui cursor tests passed\n";
    return EXIT_SUCCESS;
}
