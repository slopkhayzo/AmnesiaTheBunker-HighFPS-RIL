#include "guarded_memory.h"

#include <Windows.h>

#include <cstring>

namespace atb::memory {
namespace {

// Lowest and highest addresses a 64-bit Windows user-mode allocation can
// occupy. Anything outside is rejected before it is touched.
constexpr std::uintptr_t kLowestUserAddress = 0x10000;
constexpr std::uintptr_t kHighestUserAddress = 0x00007FFFFFFEFFFF;

struct ImageRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

ImageRange QueryMainImage() noexcept {
    const auto* const base =
        reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return {};
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return {};
    }
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage == 0) {
        return {};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    return {begin, begin + nt->OptionalHeader.SizeOfImage};
}

} // namespace

namespace detail {

int FaultFilter(const unsigned long code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION ||
                   code == EXCEPTION_IN_PAGE_ERROR ||
                   code == EXCEPTION_GUARD_PAGE
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace detail

bool IsPlausibleRange(const void* const address,
                      const std::size_t bytes) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    return bytes != 0 && begin >= kLowestUserAddress &&
           begin <= kHighestUserAddress &&
           bytes - 1 <= kHighestUserAddress - begin;
}

bool Copy(void* const destination,
          const void* const source,
          const std::size_t bytes) noexcept {
    if (!IsPlausibleRange(destination, bytes) ||
        !IsPlausibleRange(source, bytes)) {
        return false;
    }
    __try {
        std::memcpy(destination, source, bytes);
    } __except (detail::FaultFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

bool Equal(const void* const left,
           const void* const right,
           const std::size_t bytes) noexcept {
    if (!IsPlausibleRange(left, bytes) || !IsPlausibleRange(right, bytes)) {
        return false;
    }
    bool equal = false;
    __try {
        equal = std::memcmp(left, right, bytes) == 0;
    } __except (detail::FaultFilter(GetExceptionCode())) {
        return false;
    }
    return equal;
}

bool IsMainImageAddress(const void* const address) noexcept {
    static const ImageRange range = QueryMainImage();
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= range.begin && value < range.end;
}

} // namespace atb::memory
