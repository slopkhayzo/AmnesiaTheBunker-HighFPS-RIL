#include "target_mapping.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    atb::target::HostImage image{};
    std::wstring error;
    Require(!atb::target::InspectMappedPe64(nullptr, image, error) && !error.empty(),
            "null module should be rejected with a reason");

    error.clear();
    Require(atb::target::InspectMappedPe64(GetModuleHandleW(nullptr), image, error),
            "the x64 test host should pass mapped-image inspection");
    Require(image.machine == IMAGE_FILE_MACHINE_AMD64,
            "host mapping should report AMD64");
    Require(image.imageBase != 0 && image.imageSize != 0 && image.textRva != 0 &&
                image.textSize != 0 && image.sectionCount != 0,
            "host mapping should expose bounded PE identity fields");

    atb::target::HostImage supported{};
    supported.machine = IMAGE_FILE_MACHINE_AMD64;
    supported.timeDateStamp = 0x65A5248AU;
    supported.imageSize = 0x009F0000U;
    atb::target::KnownBuild build{};
    constexpr wchar_t expectedHash[] =
        L"377664ba3e6fc84d4f9b04a75a1534868467106e7135fa951167e2c223c2dbd5";
    error.clear();
    Require(atb::target::MatchKnownBuild(supported, expectedHash, build, error) &&
                build.id == atb::target::BuildId::gog_57301760292462330 &&
                build.verified,
            "the frozen GOG target identity should match case-insensitively");

    // Another executable (a different store's build, a patched one) is not
    // rejected by identity: it gets the same mapping set, unverified, and
    // the code signatures decide.
    supported.imageSize += 0x1000U;
    error.clear();
    Require(atb::target::MatchKnownBuild(supported, expectedHash, build, error) &&
                build.id == atb::target::BuildId::gog_57301760292462330 &&
                !build.verified,
            "a different executable should be accepted as unverified");
    Require(atb::target::MatchKnownBuild(supported, L"", build, error) &&
                !build.verified,
            "a missing hash should not reject the host");
    supported.machine = IMAGE_FILE_MACHINE_I386;
    Require(!atb::target::MatchKnownBuild(supported, expectedHash, build, error) &&
                build.id == atb::target::BuildId::unknown && !error.empty(),
            "a host that is not AMD64 must be rejected");

    std::cout << "PE32+ host inspection and frozen-target identity tests passed\n";
    return EXIT_SUCCESS;
}
