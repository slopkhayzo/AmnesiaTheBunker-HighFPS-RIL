#pragma once

#include <cmath>
#include <cstdint>
#include <string>

// Presentation-rate GUI cursor.
//
// The game's menus and inventory are drawn by cImGui. Its script pass runs
// once per simulation tick: the mouse position is sent in, widgets are
// evaluated, and draw commands, the cursor among them, are stored in a list.
// That list is replayed to the screen every frame, so the cursor only moves
// once per tick however fast the game renders.
//
// This layer remembers which stored command is the cursor and, around each
// replay, moves that one command to where the mouse is now and puts it back
// afterwards. Commands that follow the mouse from tick to tick, such as a
// dragged item or a tooltip, are recognised by keeping their offset from it
// and are moved the same way. Hover, focus and clicks still happen on the
// tick with the position the game read itself; no input is consumed,
// reordered or injected.
namespace atb::gui_cursor {

// One screen axis of the game's pixel-to-GUI mapping, learned from the
// positions the game itself sends: virtual = slope * pixel + offset.
struct AxisMap {
    bool hasLast = false;
    bool ready = false;
    int lastPixel = 0;
    float lastVirtual = 0.0F;
    int slopeSpan = 0; // pixel distance the slope was measured over
    double slope = 0.0;
    double offset = 0.0;

    void Observe(const int pixel, const float virtualPosition) noexcept {
        if (!std::isfinite(virtualPosition)) {
            *this = {};
            return;
        }
        if (hasLast && pixel != lastPixel) {
            const int span = pixel > lastPixel ? pixel - lastPixel
                                               : lastPixel - pixel;
            const double measured =
                (static_cast<double>(virtualPosition) - lastVirtual) /
                (static_cast<double>(pixel) - lastPixel);
            // Longer moves measure the slope better; a clearly different
            // slope (a resolution change) always replaces the old one.
            const bool different =
                ready && std::abs(measured - slope) > 0.05 * std::abs(slope);
            if (std::isfinite(measured) && measured != 0.0 &&
                (!ready || different || span >= slopeSpan)) {
                slope = measured;
                slopeSpan = different ? span : (span < 64 ? span : 64);
                ready = true;
            }
        }
        if (ready) {
            offset = static_cast<double>(virtualPosition) -
                     slope * static_cast<double>(pixel);
        }
        hasLast = true;
        lastPixel = pixel;
        lastVirtual = virtualPosition;
    }

    [[nodiscard]] double At(const double pixel) const noexcept {
        return slope * pixel + offset;
    }
};

struct Targets {
    std::uintptr_t draw = 0;                     // cImGui: replay the list
    std::uintptr_t doMouse = 0;                  // cImGui::DoMouse
    std::uintptr_t end = 0;                      // cImGui::End
    std::uintptr_t sendMousePosition = 0;        // cImGui::SendMousePosition
    std::uintptr_t sendMouseVirtualPosition = 0; // in-world GUI variant
};

struct Report {
    std::uint64_t sends = 0;          // mouse positions the game sent
    std::uint64_t sendsMatchingOs = 0;     // equal to SDL's cursor position
    std::uint64_t sendsMatchingMotion = 0; // moved by the counted motion
    std::uint64_t virtualSends = 0;   // in-world GUI positions (not previewed)
    std::uint64_t cursorsQueued = 0;  // cursor commands identified
    std::uint64_t replays = 0;        // list replays seen
    std::uint64_t moved = 0;          // replays with the cursor moved
    std::uint64_t unmapped = 0;       // replays without a usable mapping
    std::uint64_t stale = 0;          // cursor command no longer as recorded
    std::uint64_t failures = 0;       // engine memory unreadable or unwritable
    std::uint64_t followersFound = 0; // commands found following the mouse
    std::uint64_t followersMoved = 0; // of those, moved for a replay
    double largestMove = 0.0;         // GUI units
    double slopeX = 0.0;
    double slopeY = 0.0;
    int mode = 0; // 0 none, 1 OS cursor position, 2 counted motion
};

// Hooks the five cImGui functions. MinHook must be initialized and the mouse
// trace installed. Everything stays untouched if any hook fails.
[[nodiscard]] bool Install(const Targets& targets, std::wstring& error) noexcept;
[[nodiscard]] Report TakeReport() noexcept;

} // namespace atb::gui_cursor
