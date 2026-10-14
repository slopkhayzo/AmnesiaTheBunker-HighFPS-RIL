#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atb::target {

struct HostImage {
    std::uintptr_t imageBase = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t entryPointRva = 0;
    std::uint32_t textRva = 0;
    std::uint32_t textSize = 0;
    std::uint32_t importDirectoryRva = 0;
    std::uint32_t importDirectorySize = 0;
    std::uint16_t machine = 0;
    std::uint16_t sectionCount = 0;
};

enum class BuildId : std::uint32_t {
    unknown,
    gog_57301760292462330,
};

struct KnownBuild {
    BuildId id = BuildId::unknown;
    const wchar_t* name = L"unknown";
    // True when the executable is byte-for-byte the one the mappings were
    // taken from. Other executables are still accepted: every hook is then
    // decided by its own code signature.
    bool verified = false;
};

[[nodiscard]] bool InspectMappedPe64(HMODULE module,
                                     HostImage& image,
                                     std::wstring& error) noexcept;
// Chooses the mapping set for a host. The executable's hash is evidence,
// not a gate: any AMD64 host gets the known mapping set, and each mapping is
// only used if the code at its address matches its signature.
[[nodiscard]] bool MatchKnownBuild(const HostImage& image,
                                   std::wstring_view sha256,
                                   KnownBuild& build,
                                   std::wstring& error) noexcept;

} // namespace atb::target
