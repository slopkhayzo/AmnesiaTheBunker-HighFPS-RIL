#pragma once

#include <string>
#include <string_view>

namespace atb::log {

enum class Level {
    debug,
    info,
    warning,
    error,
};

[[nodiscard]] bool Initialize(const std::wstring& path);
void SetDiagnosticsEnabled(bool enabled);
[[nodiscard]] bool DiagnosticsEnabled();
void Write(Level level, std::wstring_view message);

} // namespace atb::log
