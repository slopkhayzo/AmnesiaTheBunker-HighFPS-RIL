#include "particle_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const atb::particle::Vec3 older{-2.0F, 4.0F, 8.0F};
    const atb::particle::Vec3 newer{2.0F, 8.0F, -4.0F};
    atb::particle::Vec3 result{};
    Require(atb::particle::Interpolate(older, newer, 0.0, result) &&
                result.x == older.x && result.y == older.y && result.z == older.z,
            "alpha zero did not preserve the older fixed state");
    Require(atb::particle::Interpolate(older, newer, 1.0, result) &&
                result.x == newer.x && result.y == newer.y && result.z == newer.z,
            "alpha one did not preserve the newer fixed state");
    Require(atb::particle::Interpolate(older, newer, 0.5, result) &&
                std::abs(result.x) < 1.0e-6F &&
                std::abs(result.y - 6.0F) < 1.0e-6F &&
                std::abs(result.z - 2.0F) < 1.0e-6F,
            "particle midpoint was not linearly interpolated");

    atb::particle::Vec3 invalid{};
    invalid.y = std::numeric_limits<float>::quiet_NaN();
    Require(!atb::particle::Interpolate(invalid, newer, 0.5, result),
            "non-finite endpoint was accepted");
    Require(!atb::particle::Interpolate(older, newer, -0.01, result),
            "negative alpha was accepted");
    Require(!atb::particle::Interpolate(older, newer, 1.01, result),
            "alpha above one was accepted");

    std::cout << "particle interpolation endpoint and rejection tests passed\n";
    return EXIT_SUCCESS;
}
