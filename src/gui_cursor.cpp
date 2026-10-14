#include "gui_cursor.h"

#include "guarded_memory.h"
#include "mouse_trace.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>

namespace atb::gui_cursor {
namespace {

// cImGui layout, GOG build 57301760292462330.
constexpr std::size_t kCommandsBegin = 0x300; // std::vector of draw commands
constexpr std::size_t kCommandsEnd = 0x308;
constexpr std::size_t kCommandSize = 0xA0;
constexpr std::size_t kCommandGfx = 0x00;
constexpr std::size_t kCommandPosition = 0x08; // x, y, z
constexpr std::size_t kTextsBegin = 0x318; // std::vector of text commands
constexpr std::size_t kTextsEnd = 0x320;
constexpr std::size_t kTextSize = 0x58;
constexpr std::size_t kTextFont = 0x20;
constexpr std::size_t kTextPosition = 0x28; // x, y, z
constexpr std::size_t kMouseDrawn = 0x51C;     // DoMouse ran this pass
constexpr std::size_t kShowMouseAutomatically = 0x6A8;
constexpr std::size_t kShowMouse = 0x6A9;
constexpr std::size_t kMousePosition = 0x4E48;     // newest sent position
constexpr std::size_t kMouseDrawPosition = 0x4E50; // where the cursor draws

constexpr std::size_t kMaximumCommands = 1U << 20;
constexpr int kRequiredAgreement = 3;
constexpr double kMaximumMovePixels = 2000.0;
// Commands that follow the mouse (a dragged item, a tooltip) are found by
// comparing each tick's list with the one before.
constexpr std::size_t kSeenCapacity = 768; // newest commands of a list
constexpr std::size_t kSearchWindow = 8;   // index drift between two ticks
constexpr std::size_t kMarkCapacity = 32;
constexpr float kFollowTolerance = 0.02F;  // GUI units
constexpr float kMinimumMouseMove = 1.0F;
constexpr std::uint8_t kFollowsNewest = 1; // the position scripts read
constexpr std::uint8_t kFollowsDrawn = 2;  // the position the cursor uses

using ImGuiMethod = void (*)(void* imgui);
using ImGuiDoMouse = void (*)(void* imgui,
                              void* gfx,
                              const float* offset,
                              const float* size);
using ImGuiSendMousePosition = void (*)(void* imgui,
                                        const int* position,
                                        const int* relative);
using ImGuiSendMouseVirtualPosition = void (*)(void* imgui,
                                               const float* position,
                                               const float* relative);

ImGuiMethod g_originalDraw = nullptr;
ImGuiDoMouse g_originalDoMouse = nullptr;
ImGuiMethod g_originalEnd = nullptr;
ImGuiSendMousePosition g_originalSendMousePosition = nullptr;
ImGuiSendMouseVirtualPosition g_originalSendMouseVirtualPosition = nullptr;

// Everything below belongs to the thread that sends mouse positions.
std::atomic<DWORD> g_thread{0};

// One kind of stored command: images or text.
struct CommandKind {
    std::size_t begin;
    std::size_t end;
    std::size_t size;
    std::size_t key; // the graphic or the font: what makes two commands alike
    std::size_t position;
};
constexpr std::array<CommandKind, 2> kKinds{{
    {kCommandsBegin, kCommandsEnd, kCommandSize, kCommandGfx,
     kCommandPosition},
    {kTextsBegin, kTextsEnd, kTextSize, kTextFont, kTextPosition},
}};

struct Seen {
    const void* key = nullptr;
    float x = 0.0F;
    float y = 0.0F;
    std::uint8_t follows = 0;
};

struct SeenList {
    std::array<Seen, kSeenCapacity> items{};
    std::size_t first = 0; // list index of items[0]
    std::size_t count = 0;
};

struct Mark {
    std::size_t kind = 0;
    std::size_t index = 0;
    const void* key = nullptr;
    float x = 0.0F;
    float y = 0.0F;
    std::uint8_t follows = 0;
};

struct Slot {
    void* imgui = nullptr;
    std::uint64_t lastUse = 0;

    AxisMap x;
    AxisMap y;
    bool hasSend = false;
    bool virtualDriven = false; // positions come from an in-world GUI
    int sentX = 0;
    int sentY = 0;
    float sentVirtualX = 0.0F;
    float sentVirtualY = 0.0F;
    std::int64_t observedAtSendX = 0;
    std::int64_t observedAtSendY = 0;
    int agreesWithOs = 0;
    int agreesWithMotion = 0;

    bool cursorValid = false;
    std::size_t cursorIndex = 0;
    void* cursorGfx = nullptr;
    float cursorX = 0.0F; // the command's position as the game queued it
    float cursorY = 0.0F;
    float baseX = 0.0F;   // the mouse position it was queued for
    float baseY = 0.0F;

    // The previous tick's lists and mouse positions, and this tick's
    // commands found to follow the mouse.
    std::array<SeenList, 2> seen{};
    bool hasSeen = false;
    float newestX = 0.0F;
    float newestY = 0.0F;
    float drawnX = 0.0F;
    float drawnY = 0.0F;
    std::array<Mark, kMarkCapacity> marks{};
    std::size_t markCount = 0;
};

std::array<Slot, 4> g_slots{};
SeenList g_scratch{};
std::uint64_t g_useCounter = 0;

std::atomic<std::uint64_t> g_sends{0};
std::atomic<std::uint64_t> g_sendsMatchingOs{0};
std::atomic<std::uint64_t> g_sendsMatchingMotion{0};
std::atomic<std::uint64_t> g_virtualSends{0};
std::atomic<std::uint64_t> g_cursorsQueued{0};
std::atomic<std::uint64_t> g_replays{0};
std::atomic<std::uint64_t> g_moved{0};
std::atomic<std::uint64_t> g_unmapped{0};
std::atomic<std::uint64_t> g_stale{0};
std::atomic<std::uint64_t> g_failures{0};
std::atomic<std::uint64_t> g_largestMoveMilli{0};
std::atomic<std::uint64_t> g_followersFound{0};
std::atomic<std::uint64_t> g_followersMoved{0};
std::atomic<int> g_mode{0};
std::atomic<std::int64_t> g_slopeXMicro{0};
std::atomic<std::int64_t> g_slopeYMicro{0};

bool OnOwnThread() noexcept {
    const DWORD current = GetCurrentThreadId();
    DWORD expected = 0;
    return g_thread.compare_exchange_strong(expected, current,
                                            std::memory_order_relaxed) ||
           expected == current;
}

Slot& SlotFor(void* const imgui) noexcept {
    Slot* oldest = &g_slots[0];
    for (Slot& slot : g_slots) {
        if (slot.imgui == imgui) {
            slot.lastUse = ++g_useCounter;
            return slot;
        }
        if (slot.lastUse < oldest->lastUse) {
            oldest = &slot;
        }
    }
    *oldest = {};
    oldest->imgui = imgui;
    oldest->lastUse = ++g_useCounter;
    return *oldest;
}

const std::uint8_t* Field(const void* const imgui,
                          const std::size_t offset) noexcept {
    return static_cast<const std::uint8_t*>(imgui) + offset;
}

// Number of stored commands of one kind and the address of the first.
bool ReadList(const void* const imgui,
              const CommandKind& kind,
              std::uint8_t*& begin,
              std::size_t& count) noexcept {
    std::uint8_t* end = nullptr;
    if (!memory::Read(Field(imgui, kind.begin), begin) ||
        !memory::Read(Field(imgui, kind.end), end)) {
        return false;
    }
    if (begin == nullptr || end < begin) {
        count = 0;
        return begin == end;
    }
    const std::size_t bytes = static_cast<std::size_t>(end - begin);
    count = bytes / kind.size;
    return bytes % kind.size == 0 && count <= kMaximumCommands;
}

bool ReadCommands(const void* const imgui,
                  std::uint8_t*& begin,
                  std::size_t& count) noexcept {
    return ReadList(imgui, kKinds[0], begin, count);
}

bool Near(const float left, const float right) noexcept {
    return std::abs(left - right) <= kFollowTolerance;
}

// Compares the stored commands of this tick with those of the tick before.
// A command that kept its offset from the mouse while the mouse moved is
// following it; one that followed before and still has that offset keeps
// doing so while the mouse rests.
void FindFollowers(Slot& slot) noexcept {
    slot.markCount = 0;
    float mouse[4]{};
    if (!memory::Copy(mouse, Field(slot.imgui, kMousePosition),
                      sizeof(mouse)) ||
        !std::isfinite(mouse[0]) || !std::isfinite(mouse[1]) ||
        !std::isfinite(mouse[2]) || !std::isfinite(mouse[3])) {
        slot.hasSeen = false;
        return;
    }
    const bool newestMoved =
        slot.hasSeen &&
        (std::abs(mouse[0] - slot.newestX) >= kMinimumMouseMove ||
         std::abs(mouse[1] - slot.newestY) >= kMinimumMouseMove);
    const bool drawnMoved =
        slot.hasSeen &&
        (std::abs(mouse[2] - slot.drawnX) >= kMinimumMouseMove ||
         std::abs(mouse[3] - slot.drawnY) >= kMinimumMouseMove);
    bool complete = true;
    for (std::size_t kindIndex = 0; kindIndex < kKinds.size(); ++kindIndex) {
        const CommandKind& kind = kKinds[kindIndex];
        const SeenList& before = slot.seen[kindIndex];
        SeenList& now = g_scratch;
        now.first = 0;
        now.count = 0;
        std::uint8_t* begin = nullptr;
        std::size_t count = 0;
        if (!ReadList(slot.imgui, kind, begin, count)) {
            complete = false;
            slot.seen[kindIndex].count = 0;
            continue;
        }
        now.first = count > kSeenCapacity ? count - kSeenCapacity : 0;
        for (std::size_t index = now.first; index < count; ++index) {
            Seen item{};
            float position[2]{};
            const std::uint8_t* const command = begin + index * kind.size;
            if (!memory::Read(command + kind.key, item.key) ||
                !memory::Copy(position, command + kind.position,
                              sizeof(position)) ||
                !std::isfinite(position[0]) || !std::isfinite(position[1])) {
                complete = false;
                break;
            }
            item.x = position[0];
            item.y = position[1];
            // The same command is normally at the same index; allow a few
            // places of drift for commands that came or went before it.
            for (std::size_t step = 0;
                 slot.hasSeen && item.follows == 0 && step <= 2 * kSearchWindow;
                 ++step) {
                const std::size_t distance = (step + 1) / 2;
                const bool backwards = step % 2 == 1;
                if (backwards && distance > index) {
                    continue;
                }
                const std::size_t other =
                    backwards ? index - distance : index + distance;
                if (other < before.first ||
                    other >= before.first + before.count) {
                    continue;
                }
                const Seen& old = before.items[other - before.first];
                if (old.key != item.key) {
                    continue;
                }
                if (Near(item.x - mouse[0], old.x - slot.newestX) &&
                    Near(item.y - mouse[1], old.y - slot.newestY) &&
                    (newestMoved || (old.follows & kFollowsNewest) != 0)) {
                    item.follows |= kFollowsNewest;
                }
                if (Near(item.x - mouse[2], old.x - slot.drawnX) &&
                    Near(item.y - mouse[3], old.y - slot.drawnY) &&
                    (drawnMoved || (old.follows & kFollowsDrawn) != 0)) {
                    item.follows |= kFollowsDrawn;
                }
            }
            now.items[now.count++] = item;
            const bool isCursor = kindIndex == 0 && slot.cursorValid &&
                                  index == slot.cursorIndex;
            if (item.follows != 0 && !isCursor &&
                slot.markCount < kMarkCapacity) {
                slot.marks[slot.markCount++] = {kindIndex, index, item.key,
                                                item.x, item.y, item.follows};
                g_followersFound.fetch_add(1, std::memory_order_relaxed);
            }
        }
        slot.seen[kindIndex] = now;
    }
    slot.newestX = mouse[0];
    slot.newestY = mouse[1];
    slot.drawnX = mouse[2];
    slot.drawnY = mouse[3];
    slot.hasSeen = complete;
    if (!complete) {
        slot.markCount = 0;
        g_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

// Records command `index` as the cursor, queued for the mouse position the
// game is drawing at.
void RecordCursor(Slot& slot, const std::size_t index) noexcept {
    slot.cursorValid = false;
    std::uint8_t* begin = nullptr;
    std::size_t count = 0;
    if (!ReadCommands(slot.imgui, begin, count) || index >= count) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint8_t* const command = begin + index * kCommandSize;
    float position[2]{};
    float base[2]{};
    if (!memory::Read(command + kCommandGfx, slot.cursorGfx) ||
        !memory::Copy(position, command + kCommandPosition, sizeof(position)) ||
        !memory::Copy(base, Field(slot.imgui, kMouseDrawPosition),
                      sizeof(base)) ||
        !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
        !std::isfinite(base[0]) || !std::isfinite(base[1])) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot.cursorIndex = index;
    slot.cursorX = position[0];
    slot.cursorY = position[1];
    slot.baseX = base[0];
    slot.baseY = base[1];
    slot.cursorValid = true;
    g_cursorsQueued.fetch_add(1, std::memory_order_relaxed);
}

void HookedSendMousePosition(void* const imgui,
                             const int* const position,
                             const int* const relative) {
    g_originalSendMousePosition(imgui, position, relative);
    if (imgui == nullptr || position == nullptr || !OnOwnThread()) {
        return;
    }
    g_sends.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = SlotFor(imgui);
    float sent[2]{};
    if (!memory::Copy(sent, Field(imgui, kMousePosition), sizeof(sent))) {
        g_failures.fetch_add(1, std::memory_order_relaxed);
        slot.hasSend = false;
        return;
    }
    const int x = position[0];
    const int y = position[1];
    const mouse::CursorState os = mouse::ReadCursor();
    if (os.available) {
        // Two independent checks that the plugin sees the mouse the way the
        // game does; the preview needs one of them to hold.
        if (!os.relativeMode && os.x == x && os.y == y) {
            ++slot.agreesWithOs;
            g_sendsMatchingOs.fetch_add(1, std::memory_order_relaxed);
        } else {
            slot.agreesWithOs = 0;
        }
        if (slot.hasSend && !slot.virtualDriven &&
            x - slot.sentX == os.observedX - slot.observedAtSendX &&
            y - slot.sentY == os.observedY - slot.observedAtSendY) {
            ++slot.agreesWithMotion;
            g_sendsMatchingMotion.fetch_add(1, std::memory_order_relaxed);
        } else {
            slot.agreesWithMotion = 0;
        }
        slot.observedAtSendX = os.observedX;
        slot.observedAtSendY = os.observedY;
    }
    slot.x.Observe(x, sent[0]);
    slot.y.Observe(y, sent[1]);
    slot.sentX = x;
    slot.sentY = y;
    slot.sentVirtualX = sent[0];
    slot.sentVirtualY = sent[1];
    slot.hasSend = os.available;
    slot.virtualDriven = false;
}

void HookedSendMouseVirtualPosition(void* const imgui,
                                    const float* const position,
                                    const float* const relative) {
    g_originalSendMouseVirtualPosition(imgui, position, relative);
    if (imgui == nullptr || !OnOwnThread()) {
        return;
    }
    // A GUI on a surface in the world: its cursor follows the view, not the
    // mouse position, and is left to the game.
    g_virtualSends.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = SlotFor(imgui);
    slot.virtualDriven = true;
    slot.hasSend = false;
    slot.agreesWithOs = 0;
    slot.agreesWithMotion = 0;
}

void HookedDoMouse(void* const imgui,
                   void* const gfx,
                   const float* const offset,
                   const float* const size) {
    std::uint8_t* begin = nullptr;
    std::size_t before = 0;
    const bool tracked = imgui != nullptr && OnOwnThread() &&
                         ReadCommands(imgui, begin, before);
    g_originalDoMouse(imgui, gfx, offset, size);
    if (!tracked) {
        return;
    }
    std::size_t after = 0;
    Slot& slot = SlotFor(imgui);
    if (ReadCommands(imgui, begin, after) && after == before + 1) {
        RecordCursor(slot, before);
    } else {
        slot.cursorValid = false;
    }
}

void HookedEnd(void* const imgui) {
    std::uint8_t* begin = nullptr;
    std::size_t before = 0;
    std::uint8_t drawn = 0;
    std::uint8_t show = 0;
    std::uint8_t automatic = 0;
    const bool tracked =
        imgui != nullptr && OnOwnThread() &&
        ReadCommands(imgui, begin, before) &&
        memory::Read(Field(imgui, kMouseDrawn), drawn) &&
        memory::Read(Field(imgui, kShowMouse), show) &&
        memory::Read(Field(imgui, kShowMouseAutomatically), automatic);
    g_originalEnd(imgui);
    if (!tracked) {
        return;
    }
    Slot& slot = SlotFor(imgui);
    // The script may have queued the cursor itself through DoMouse during
    // this pass; otherwise End queues the default cursor first, if any.
    if (drawn == 0) {
        std::size_t after = 0;
        if (show != 0 && automatic != 0 && ReadCommands(imgui, begin, after) &&
            after > before) {
            RecordCursor(slot, before);
        } else {
            slot.cursorValid = false;
        }
    }
    FindFollowers(slot);
}

// Where the mouse is now, in this GUI's units. Returns the mode used.
int LivePosition(const Slot& slot, double& x, double& y) noexcept {
    if (!slot.hasSend || slot.virtualDriven || !slot.x.ready ||
        !slot.y.ready) {
        return 0;
    }
    const mouse::CursorState os = mouse::ReadCursor();
    if (!os.available) {
        return 0;
    }
    if (slot.agreesWithOs >= kRequiredAgreement && !os.relativeMode) {
        x = slot.x.At(os.x);
        y = slot.y.At(os.y);
        return 1;
    }
    if (slot.agreesWithMotion >= kRequiredAgreement) {
        x = slot.sentVirtualX +
            slot.x.slope *
                static_cast<double>(os.observedX - slot.observedAtSendX);
        y = slot.sentVirtualY +
            slot.y.slope *
                static_cast<double>(os.observedY - slot.observedAtSendY);
        return 2;
    }
    return 0;
}

// A stored position temporarily replaced for one replay.
struct Staged {
    std::uint8_t* address = nullptr;
    float queued[2]{};
};

// Moves one command for the coming replay if it is still the one recorded:
// same place in its list, same graphic or font, same queued position.
bool Stage(const Slot& slot,
           const std::size_t kindIndex,
           const std::size_t index,
           const void* const key,
           const float x,
           const float y,
           const double moveX,
           const double moveY,
           Staged& staged) noexcept {
    const CommandKind& kind = kKinds[kindIndex];
    std::uint8_t* begin = nullptr;
    std::size_t count = 0;
    if (!ReadList(slot.imgui, kind, begin, count) || index >= count) {
        return false;
    }
    std::uint8_t* const command = begin + index * kind.size;
    const void* currentKey = nullptr;
    if (!memory::Read(command + kind.key, currentKey) ||
        !memory::Copy(staged.queued, command + kind.position,
                      sizeof(staged.queued)) ||
        currentKey != key || staged.queued[0] != x || staged.queued[1] != y) {
        return false;
    }
    const float moved[2] = {static_cast<float>(x + moveX),
                            static_cast<float>(y + moveY)};
    if (!memory::Copy(command + kind.position, moved, sizeof(moved))) {
        return false;
    }
    staged.address = command + kind.position;
    return true;
}

bool UsableMove(const Slot& slot,
                const double moveX,
                const double moveY) noexcept {
    return std::isfinite(moveX) && std::isfinite(moveY) &&
           std::abs(moveX) <= kMaximumMovePixels * std::abs(slot.x.slope) &&
           std::abs(moveY) <= kMaximumMovePixels * std::abs(slot.y.slope);
}

void HookedDraw(void* const imgui) {
    if (imgui == nullptr || !OnOwnThread()) {
        g_originalDraw(imgui);
        return;
    }
    g_replays.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = SlotFor(imgui);
    if (!slot.cursorValid && slot.markCount == 0) {
        g_originalDraw(imgui);
        return;
    }
    mouse::PumpForFrame();
    double liveX = 0.0;
    double liveY = 0.0;
    const int mode = LivePosition(slot, liveX, liveY);
    g_mode.store(mode, std::memory_order_relaxed);
    if (mode == 0) {
        g_unmapped.fetch_add(1, std::memory_order_relaxed);
        g_originalDraw(imgui);
        return;
    }
    std::array<Staged, kMarkCapacity + 1> staged{};
    std::size_t stagedCount = 0;
    bool cursorMoved = false;
    double largest = 0.0;
    if (slot.cursorValid) {
        const double moveX = liveX - slot.baseX;
        const double moveY = liveY - slot.baseY;
        if (!UsableMove(slot, moveX, moveY)) {
            g_unmapped.fetch_add(1, std::memory_order_relaxed);
        } else if (moveX != 0.0 || moveY != 0.0) {
            if (Stage(slot, 0, slot.cursorIndex, slot.cursorGfx, slot.cursorX,
                      slot.cursorY, moveX, moveY, staged[stagedCount])) {
                ++stagedCount;
                cursorMoved = true;
                largest = std::max(std::abs(moveX), std::abs(moveY));
            } else {
                slot.cursorValid = false;
                g_stale.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    std::size_t followersMoved = 0;
    for (std::size_t index = 0; index < slot.markCount; ++index) {
        const Mark& mark = slot.marks[index];
        const bool newest = (mark.follows & kFollowsNewest) != 0;
        const double moveX = liveX - (newest ? slot.newestX : slot.drawnX);
        const double moveY = liveY - (newest ? slot.newestY : slot.drawnY);
        if (!UsableMove(slot, moveX, moveY) ||
            (moveX == 0.0 && moveY == 0.0)) {
            continue;
        }
        if (Stage(slot, mark.kind, mark.index, mark.key, mark.x, mark.y, moveX,
                  moveY, staged[stagedCount])) {
            ++stagedCount;
            ++followersMoved;
        } else {
            // The lists changed since the scan: leave the rest to the game.
            slot.markCount = 0;
            g_stale.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    g_originalDraw(imgui);
    // Put the queued values back; the lists are otherwise left untouched.
    bool restored = true;
    for (std::size_t index = 0; index < stagedCount; ++index) {
        restored &= memory::Copy(staged[index].address, staged[index].queued,
                                 sizeof(staged[index].queued));
    }
    if (!restored) {
        slot.cursorValid = false;
        slot.markCount = 0;
        g_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_followersMoved.fetch_add(followersMoved, std::memory_order_relaxed);
    if (!cursorMoved) {
        return;
    }
    g_moved.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t milli = static_cast<std::uint64_t>(largest * 1000.0);
    std::uint64_t seen = g_largestMoveMilli.load(std::memory_order_relaxed);
    while (seen < milli &&
           !g_largestMoveMilli.compare_exchange_weak(
               seen, milli, std::memory_order_relaxed)) {
    }
    g_slopeXMicro.store(static_cast<std::int64_t>(slot.x.slope * 1.0e6),
                        std::memory_order_relaxed);
    g_slopeYMicro.store(static_cast<std::int64_t>(slot.y.slope * 1.0e6),
                        std::memory_order_relaxed);
}

struct HookEntry {
    std::uintptr_t target;
    void* detour;
    void** original;
};

} // namespace

bool Install(const Targets& targets, std::wstring& error) noexcept {
    error.clear();
    const std::array<HookEntry, 5> hooks{{
        {targets.draw, reinterpret_cast<void*>(&HookedDraw),
         reinterpret_cast<void**>(&g_originalDraw)},
        {targets.doMouse, reinterpret_cast<void*>(&HookedDoMouse),
         reinterpret_cast<void**>(&g_originalDoMouse)},
        {targets.end, reinterpret_cast<void*>(&HookedEnd),
         reinterpret_cast<void**>(&g_originalEnd)},
        {targets.sendMousePosition,
         reinterpret_cast<void*>(&HookedSendMousePosition),
         reinterpret_cast<void**>(&g_originalSendMousePosition)},
        {targets.sendMouseVirtualPosition,
         reinterpret_cast<void*>(&HookedSendMouseVirtualPosition),
         reinterpret_cast<void**>(&g_originalSendMouseVirtualPosition)},
    }};
    for (const HookEntry& hook : hooks) {
        if (hook.target == 0) {
            error = L"GUI cursor mapping is incomplete";
            return false;
        }
    }
    // All five or none: create, then enable, undoing everything on failure.
    std::size_t created = 0;
    MH_STATUS status = MH_OK;
    for (; created < hooks.size() && status == MH_OK; ++created) {
        status = MH_CreateHook(reinterpret_cast<void*>(hooks[created].target),
                               hooks[created].detour, hooks[created].original);
        if (status != MH_OK) {
            break;
        }
    }
    if (status == MH_OK) {
        for (const HookEntry& hook : hooks) {
            status = MH_QueueEnableHook(reinterpret_cast<void*>(hook.target));
            if (status != MH_OK) {
                break;
            }
        }
        if (status == MH_OK) {
            status = MH_ApplyQueued();
        }
    }
    if (status != MH_OK) {
        for (std::size_t index = 0; index < created; ++index) {
            (void)MH_RemoveHook(reinterpret_cast<void*>(hooks[index].target));
        }
        error = L"Could not hook the GUI cursor functions (MinHook status " +
                std::to_wstring(status) + L")";
        return false;
    }
    return true;
}

Report TakeReport() noexcept {
    Report report{};
    report.sends = g_sends.exchange(0, std::memory_order_acq_rel);
    report.sendsMatchingOs =
        g_sendsMatchingOs.exchange(0, std::memory_order_acq_rel);
    report.sendsMatchingMotion =
        g_sendsMatchingMotion.exchange(0, std::memory_order_acq_rel);
    report.virtualSends = g_virtualSends.exchange(0, std::memory_order_acq_rel);
    report.cursorsQueued =
        g_cursorsQueued.exchange(0, std::memory_order_acq_rel);
    report.replays = g_replays.exchange(0, std::memory_order_acq_rel);
    report.moved = g_moved.exchange(0, std::memory_order_acq_rel);
    report.unmapped = g_unmapped.exchange(0, std::memory_order_acq_rel);
    report.stale = g_stale.exchange(0, std::memory_order_acq_rel);
    report.failures = g_failures.exchange(0, std::memory_order_acq_rel);
    report.followersFound =
        g_followersFound.exchange(0, std::memory_order_acq_rel);
    report.followersMoved =
        g_followersMoved.exchange(0, std::memory_order_acq_rel);
    report.largestMove =
        static_cast<double>(
            g_largestMoveMilli.exchange(0, std::memory_order_acq_rel)) /
        1000.0;
    report.slopeX =
        static_cast<double>(g_slopeXMicro.load(std::memory_order_relaxed)) /
        1.0e6;
    report.slopeY =
        static_cast<double>(g_slopeYMicro.load(std::memory_order_relaxed)) /
        1.0e6;
    report.mode = g_mode.load(std::memory_order_relaxed);
    return report;
}

} // namespace atb::gui_cursor
