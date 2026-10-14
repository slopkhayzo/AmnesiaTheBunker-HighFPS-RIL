#pragma once

#include <cstddef>
#include <cstdint>
#include <excpt.h>

// Hot-path access to engine memory without operating-system memory queries.
// Every helper is a fixed-bounds check plus a structured-exception guard, so
// the non-faulting path costs no system call and a stale pointer still fails
// closed instead of terminating the game.
namespace atb::memory {

[[nodiscard]] bool IsPlausibleRange(const void* address,
                                    std::size_t bytes) noexcept;
[[nodiscard]] bool Copy(void* destination,
                        const void* source,
                        std::size_t bytes) noexcept;
[[nodiscard]] bool Equal(const void* left,
                         const void* right,
                         std::size_t bytes) noexcept;
// True only for addresses inside the host executable image. The image range
// is read once from the loaded PE header.
[[nodiscard]] bool IsMainImageAddress(const void* address) noexcept;

namespace detail {
[[nodiscard]] int FaultFilter(unsigned long code) noexcept;
} // namespace detail

// Runs a short read-only scan of engine memory under the same fault guard.
// The callable must not own objects that need unwinding.
template <typename Function>
[[nodiscard]] bool Guarded(Function&& function) noexcept {
    __try {
        function();
    } __except (detail::FaultFilter(_exception_code())) {
        return false;
    }
    return true;
}

template <typename Value>
[[nodiscard]] bool Read(const void* const address, Value& value) noexcept {
    return Copy(&value, address, sizeof(Value));
}

template <typename Value>
[[nodiscard]] bool Write(void* const address, const Value& value) noexcept {
    return Copy(address, &value, sizeof(Value));
}

} // namespace atb::memory
