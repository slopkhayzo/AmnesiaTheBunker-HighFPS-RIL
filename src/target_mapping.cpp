#include "target_mapping.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>

namespace atb::target {
namespace {

bool RangeIsInImage(const std::uint32_t imageSize,
                    const std::uint32_t offset,
                    const std::uint32_t size) noexcept {
    return offset <= imageSize && size <= imageSize - offset;
}

bool EqualsCaseInsensitive(const std::wstring_view left,
                           const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const wchar_t a, const wchar_t b) {
                          return std::towupper(a) == std::towupper(b);
                      });
}

constexpr std::uint32_t kGogBuildTimestamp = 0x65A5248AU;
constexpr std::uint32_t kGogBuildImageSize = 0x009F0000U;
constexpr std::wstring_view kGogBuildSha256 =
    L"377664BA3E6FC84D4F9B04A75A1534868467106E7135FA951167E2C223C2DBD5";
// Steam's AmnesiaTheBunker.exe. Its AmnesiaTheBunker_NoSteam.exe is
// byte-for-byte the GOG executable.
constexpr std::uint32_t kSteamBuildTimestamp = 0x65A52182U;
constexpr std::uint32_t kSteamBuildImageSize = 0x00A23000U;
constexpr std::wstring_view kSteamBuildSha256 =
    L"0FF4A67D332FE8490F69D3A8219BACEF120195729A7FCD62B1399455256AE519";

} // namespace

bool InspectMappedPe64(const HMODULE module,
                       HostImage& image,
                       std::wstring& error) noexcept {
    image = {};
    error.clear();
    if (module == nullptr) {
        error = L"Main executable module is unavailable";
        return false;
    }

    const auto* const base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::uint32_t>(dos->e_lfanew) > 1024U * 1024U) {
        error = L"Invalid DOS header in mapped host image";
        return false;
    }

    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = L"Host is not a valid x64 PE32+ image";
        return false;
    }
    if (nt->OptionalHeader.SizeOfImage < nt->OptionalHeader.SizeOfHeaders ||
        nt->OptionalHeader.SizeOfImage == 0) {
        error = L"Host PE image size is invalid";
        return false;
    }

    const std::uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto* const sections = IMAGE_FIRST_SECTION(nt);
    const std::size_t sectionTableOffset =
        static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(sections) - base);
    const std::size_t sectionTableSize =
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (sectionTableOffset > nt->OptionalHeader.SizeOfHeaders ||
        sectionTableSize > nt->OptionalHeader.SizeOfHeaders - sectionTableOffset) {
        error = L"Host PE section table lies outside the mapped headers";
        return false;
    }

    const IMAGE_SECTION_HEADER* text = nullptr;
    unsigned int textMatches = 0;
    for (unsigned int index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const std::uint32_t extent =
            section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (!RangeIsInImage(imageSize, section.VirtualAddress, extent)) {
            error = L"Host PE section lies outside the mapped image";
            return false;
        }
        if (std::memcmp(section.Name, ".text", 5) == 0) {
            ++textMatches;
            text = &section;
        }
    }
    if (textMatches != 1 || text == nullptr ||
        (text->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
        error = L"Expected exactly one executable .text section";
        return false;
    }

    const IMAGE_DATA_DIRECTORY imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if ((imports.VirtualAddress == 0) != (imports.Size == 0) ||
        (imports.VirtualAddress != 0 &&
         !RangeIsInImage(imageSize, imports.VirtualAddress, imports.Size))) {
        error = L"Host PE import directory is invalid";
        return false;
    }

    image.imageBase = reinterpret_cast<std::uintptr_t>(base);
    image.imageSize = imageSize;
    image.timeDateStamp = nt->FileHeader.TimeDateStamp;
    image.entryPointRva = nt->OptionalHeader.AddressOfEntryPoint;
    image.textRva = text->VirtualAddress;
    image.textSize = text->Misc.VirtualSize;
    image.importDirectoryRva = imports.VirtualAddress;
    image.importDirectorySize = imports.Size;
    image.machine = nt->FileHeader.Machine;
    image.sectionCount = nt->FileHeader.NumberOfSections;
    return true;
}

bool MatchKnownBuild(const HostImage& image,
                     const std::wstring_view sha256,
                     KnownBuild& build,
                     std::wstring& error) noexcept {
    build = {};
    error.clear();
    if (image.machine != IMAGE_FILE_MACHINE_AMD64) {
        error = L"Known Bunker builds require an AMD64 host";
        return false;
    }
    if (image.timeDateStamp == kSteamBuildTimestamp &&
        image.imageSize == kSteamBuildImageSize) {
        build.id = BuildId::steam_13346472;
        if (EqualsCaseInsensitive(sha256, kSteamBuildSha256)) {
            build.name = L"Steam build 13346472 (1.31-era executable)";
            build.verified = true;
            return true;
        }
        build.name = L"an executable laid out like Steam build 13346472 but "
                     L"not identical to it; each feature is decided by its "
                     L"code signatures";
        return true;
    }
    build.id = BuildId::gog_57301760292462330;
    if (image.timeDateStamp == kGogBuildTimestamp &&
        image.imageSize == kGogBuildImageSize &&
        EqualsCaseInsensitive(sha256, kGogBuildSha256)) {
        build.name = L"GOG build 57301760292462330 (1.31-era executable)";
        build.verified = true;
        return true;
    }
    build.name = L"an executable other than the known GOG and Steam builds; "
                 L"each feature is decided by its code signatures";
    return true;
}

} // namespace atb::target
