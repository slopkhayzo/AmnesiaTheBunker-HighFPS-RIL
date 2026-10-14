#include "guarded_memory.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    std::array<std::uint8_t, 64> source{};
    std::array<std::uint8_t, 64> destination{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::uint8_t>(index * 3U + 1U);
    }
    Require(atb::memory::Copy(destination.data(), source.data(),
                              source.size()) &&
                destination == source,
            "ordinary readable memory was not copied");
    Require(atb::memory::Equal(destination.data(), source.data(),
                               source.size()),
            "identical ranges did not compare equal");
    destination[5] ^= 0xFFU;
    Require(!atb::memory::Equal(destination.data(), source.data(),
                                source.size()),
            "different ranges compared equal");

    std::uint32_t value = 0;
    Require(!atb::memory::Read(nullptr, value) &&
                !atb::memory::Read(reinterpret_cast<const void*>(
                                       std::uintptr_t{0x8000}),
                                   value) &&
                !atb::memory::Read(reinterpret_cast<const void*>(UINTPTR_MAX),
                                   value) &&
                !atb::memory::Read(reinterpret_cast<const void*>(
                                       std::uintptr_t{0xFFFF800000000000}),
                                   value),
            "null, low, overflowing, or kernel addresses were accepted");
    Require(!atb::memory::Copy(destination.data(), source.data(), 0),
            "an empty range was accepted");

    constexpr std::size_t pageSize = 0x1000;
    auto* const pages = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, pageSize * 3, MEM_RESERVE, PAGE_NOACCESS));
    Require(pages != nullptr, "could not reserve the fault fixture");
    Require(VirtualAlloc(pages, pageSize, MEM_COMMIT, PAGE_READWRITE) != nullptr &&
                VirtualAlloc(pages + pageSize, pageSize, MEM_COMMIT,
                             PAGE_READONLY) != nullptr,
            "could not commit the fault fixture");

    // Page 0 is writable, page 1 is read-only, page 2 is reserved only.
    Require(!atb::memory::Read(pages + pageSize * 2, value),
            "an uncommitted page was read without failing closed");
    Require(!atb::memory::Copy(destination.data(),
                               pages + pageSize * 2 - 8, 16),
            "a range crossing into an uncommitted page did not fail closed");
    Require(atb::memory::Read(pages + pageSize, value),
            "a read-only page could not be read");
    Require(!atb::memory::Write(pages + pageSize, value),
            "a write to a read-only page did not fail closed");
    Require(atb::memory::Write(pages, std::uint32_t{0x12345678U}) &&
                atb::memory::Read(pages, value) && value == 0x12345678U,
            "a writable page did not round-trip");
    Require(!atb::memory::Equal(pages + pageSize * 2, source.data(), 8),
            "comparing an uncommitted page did not fail closed");

    std::uint32_t sum = 0;
    Require(atb::memory::Guarded([&]() noexcept {
                for (std::size_t index = 0; index < 16; ++index) {
                    sum += pages[index];
                }
            }),
            "a valid guarded scan reported a fault");
    volatile const std::uint8_t* const unmapped = pages + pageSize * 2;
    Require(!atb::memory::Guarded([&]() noexcept { sum += unmapped[0]; }),
            "a faulting guarded scan did not fail closed");

    // Decommitting after a successful read models the object-lifetime race
    // that a protection query cannot close.
    Require(VirtualFree(pages, pageSize, MEM_DECOMMIT) != 0,
            "could not decommit the fault fixture");
    Require(!atb::memory::Read(pages, value),
            "memory released after validation did not fail closed");
    Require(VirtualFree(pages, 0, MEM_RELEASE) != 0,
            "could not release the fault fixture");

    Require(atb::memory::IsMainImageAddress(
                reinterpret_cast<const void*>(&Require)) &&
                !atb::memory::IsMainImageAddress(source.data()) &&
                !atb::memory::IsMainImageAddress(nullptr) &&
                !atb::memory::IsMainImageAddress(
                    reinterpret_cast<const void*>(&VirtualAlloc)),
            "host-image classification was wrong");

    std::cout << "guarded memory tests passed\n";
    return EXIT_SUCCESS;
}
