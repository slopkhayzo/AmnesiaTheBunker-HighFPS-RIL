#include "frame_trace.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <new>

namespace atb::frame_trace {
namespace {

// About 45 seconds at 350 FPS between two writer passes.
constexpr std::size_t kCapacity = 16384;
constexpr DWORD kWriterIntervalMilliseconds = 500;

std::unique_ptr<Record[]> g_ring;
std::atomic<std::uint64_t> g_written{0};  // records published by the game
std::atomic<std::uint64_t> g_consumed{0}; // records taken by the writer
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<bool> g_started{false};
HANDLE g_file = INVALID_HANDLE_VALUE;

Record g_current{};
std::int64_t g_frameStartQpc = 0;

std::uint32_t SinceFrameStart(const std::int64_t now) noexcept {
    const std::int64_t elapsed = now - g_frameStartQpc;
    if (g_frameStartQpc == 0 || elapsed <= 0) {
        return 0;
    }
    return elapsed > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(elapsed);
}

DWORD WINAPI WriterThread(void*) {
    for (;;) {
        Sleep(kWriterIntervalMilliseconds);
        std::uint64_t consumed = g_consumed.load(std::memory_order_relaxed);
        const std::uint64_t written = g_written.load(std::memory_order_acquire);
        while (consumed != written) {
            const std::size_t index = static_cast<std::size_t>(consumed % kCapacity);
            const std::size_t run = static_cast<std::size_t>(
                std::min<std::uint64_t>(written - consumed, kCapacity - index));
            DWORD bytes = 0;
            if (!WriteFile(g_file, &g_ring[index],
                           static_cast<DWORD>(run * sizeof(Record)), &bytes,
                           nullptr)) {
                return 1;
            }
            consumed += run;
            g_consumed.store(consumed, std::memory_order_release);
        }
    }
}

} // namespace

bool Start(const std::int64_t qpcFrequency, std::wstring& error) noexcept {
    error.clear();
    if (g_started.load(std::memory_order_acquire)) {
        return true;
    }
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Start), &module)) {
        error = L"Could not identify the plugin module";
        return false;
    }
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = L"Could not read the plugin path";
        return false;
    }
    std::wstring filePath(path, length);
    const std::size_t dot = filePath.find_last_of(L'.');
    const std::size_t slash = filePath.find_last_of(L"\\/");
    if (dot != std::wstring::npos &&
        (slash == std::wstring::npos || dot > slash)) {
        filePath.erase(dot);
    }
    filePath += L".frames.bin";

    g_ring.reset(new (std::nothrow) Record[kCapacity]);
    if (!g_ring) {
        error = L"Could not allocate the frame ring";
        return false;
    }
    g_file = CreateFileW(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        error = L"Could not create " + filePath;
        return false;
    }
    FileHeader header{};
    header.qpcFrequency = qpcFrequency;
    DWORD bytes = 0;
    if (!WriteFile(g_file, &header, sizeof(header), &bytes, nullptr)) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        error = L"Could not write " + filePath;
        return false;
    }
    const HANDLE thread =
        CreateThread(nullptr, 0, &WriterThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        error = L"Could not start the frame-trace writer";
        return false;
    }
    (void)SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(thread);
    g_started.store(true, std::memory_order_release);
    return true;
}

void Mark(const Stamp stamp) noexcept {
    if (!g_started.load(std::memory_order_relaxed)) {
        return;
    }
    const auto index = static_cast<std::size_t>(stamp);
    // Begin stamps keep their first value of the frame, end stamps the last.
    const bool first = stamp == Stamp::fixedBegin ||
                       stamp == Stamp::renderBegin ||
                       stamp == Stamp::paceWaitEnd ||
                       stamp == Stamp::renderOriginal ||
                       stamp == Stamp::swapEnter;
    if (first && g_current.stamps[index] != 0) {
        return;
    }
    LARGE_INTEGER now{};
    if (QueryPerformanceCounter(&now)) {
        g_current.stamps[index] = SinceFrameStart(now.QuadPart);
    }
}

void CountFixedUpdate() noexcept {
    if (g_started.load(std::memory_order_relaxed) &&
        g_current.fixedUpdates != std::numeric_limits<std::uint16_t>::max()) {
        ++g_current.fixedUpdates;
    }
}

void SetView(const float viewYaw,
             const float viewPitch,
             const float leadYaw,
             const float leadPitch,
             const bool leadApplied,
             const float alpha,
             const std::uint64_t serial) noexcept {
    if (!g_started.load(std::memory_order_relaxed) ||
        (g_current.flags & 1U) != 0) {
        return;
    }
    g_current.viewYaw = viewYaw;
    g_current.viewPitch = viewPitch;
    g_current.leadYaw = leadYaw;
    g_current.leadPitch = leadPitch;
    g_current.alpha = alpha;
    g_current.serial = static_cast<std::uint32_t>(serial);
    g_current.flags = static_cast<std::uint16_t>(
        g_current.flags | 1U | (leadApplied ? 2U : 0U));
}

void MarkGameplay() noexcept {
    if (g_started.load(std::memory_order_relaxed)) {
        g_current.flags = static_cast<std::uint16_t>(g_current.flags | 4U);
    }
}

void SetFramesPerTick(const unsigned int framesPerTick) noexcept {
    if (g_started.load(std::memory_order_relaxed)) {
        g_current.flags = static_cast<std::uint16_t>(
            (g_current.flags & 0xFFU) | (std::min(framesPerTick, 255U) << 8));
    }
}

void EndFrame(const std::int64_t presentedQpc) noexcept {
    if (!g_started.load(std::memory_order_relaxed)) {
        return;
    }
    g_current.presentedQpc = presentedQpc;
    const std::uint64_t written = g_written.load(std::memory_order_relaxed);
    if (written - g_consumed.load(std::memory_order_acquire) < kCapacity) {
        g_ring[static_cast<std::size_t>(written % kCapacity)] = g_current;
        g_written.store(written + 1, std::memory_order_release);
    } else {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_current = {};
    g_frameStartQpc = presentedQpc;
}

std::uint64_t TakeDropped() noexcept {
    return g_dropped.exchange(0, std::memory_order_acq_rel);
}

} // namespace atb::frame_trace
